# The Pi cluster's results

What the sessions on the Raspberry Pi cluster measured and found, from the
second session on, newest last.
How to run a session is the runbook, `docs/pi-cluster.md`; the raw outputs are
under `docs/design/evidence/pi-cluster/`, a directory a session, but for v0.2
step 4's, which are under `docs/design/evidence/v0.2-step4/`.

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
MiB/s in all, four links at line rate. **Bound against unbound at 4 × 4**: the
remote puts, gets and fetch-adds, and one PE's put stream, are all within 5% of
each other (the 8-byte put 168.6 against 170.5 µs), but 19 of the 66 lines the
two runs share (`bench-4x4.txt` against `bench-4x4-unbound.txt`) are not.
Unbound the barrier took 191.6 µs against 177.7 (7.8% longer), an 8-byte
fcollect 288.2 against 257.1 (12%), a 4 KiB fcollect 839.6 against 704.8 (19%),
and the node-local 64 KiB put stream moved 12,389 MiB/s against 31,816. The
agent thread sharing a core with its PE costs the 8-byte put and get nothing
measurable; what it costs a pipelined point-to-point pattern is measured below
(the night of 2026-09-27).

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

## The pacer's fourth phase (2026-09-27)

`task-stress-phase4`, 4 × 4, release: three hours of `mix` at `b252b28` by
`pi-soak-phase4a.sh`, then the tree that merged, `3d5bbe1`, by
`pi-runs-phase4b.sh`, about five and a half hours of cluster time. Outputs,
scripts and the nodes' state:
`docs/design/evidence/pi-cluster/2026-09-27/pacer/phase4/`, a directory a
commit, the long outputs gzipped; the record:
`docs/decisions/2026-09-27-evening.md`. The nodes had been up since 10:09.

**Every run ended `failures=0`**:

| Run | What it did |
|---|---|
| `mix`, 3 h at `b252b28` | 916 periods: four teams of four, one PE of each on every node, each of five exercises on some team about 550 times; 229 world periods of `sort` and `alltoall`; 1.53 TB sent between the nodes |
| `mix`, 1 h at `3d5bbe1` | 305 periods; 503 GB sent |
| `waits`, 5 min | 583,288 rounds, every wait and test form of every type 3,472 times over; 124,000 words a second checked |
| `deprecated`, 5 min | 265,439 rounds, every Annex F case on every active set 721 times over |
| the quick runs, `start_pes` | the default set in 7 s; `mix`; `deprecated`'s whole cycle; the unprefixed routines |

**The first attempt at `mix` hung** seven minutes in, every node's load at
zero: `osoe-stat` on each node showed PEs in different collectives at one
epoch - the tool's own fault, a team exercise calling the world's barrier
(the record's item 2); its output and each node's `osoe-stat` are kept.

**Retransmission timeouts**: the three hours of `mix` added 87 across the
four nodes, the hour at `3d5bbe1` 33 - about one every two minutes, where the
day's single exercises had added none or a handful. They are F1's, above;
which of `mix`'s periods raises them is not yet known.

**The counted builds** at `3d5bbe1`: 792 rounds of every exercise, a `mix`
and the `start_pes` program call 1,792 of the 1,794 routines declared - all
but `shmem_finalize`, called after the count is written, and
`shmem_global_exit`, which no clean run calls.

## The author's questions, measured (2026-09-27, night)

4 × 4, release: main's code (`3d5bbe1`) for the threads and the gets, the
branch `pacer-decisions` (`6ac31fc`) for the sort's digits and `mix` with
`waits`; about an hour of cluster time. Each node's threads were read from
`/proc` twice a window - time running, time runnable but waiting, the
switches each thread chose and those it did not - and some runs changed the
first PE's agent partway through with `taskset` or `sched_setattr`. Scripts,
samples and outputs: `docs/design/evidence/pi-cluster/2026-09-27/agent-placement/`;
the record: `docs/decisions/2026-09-27-night.md`. The nodes have one thread a
core (Cortex-A76, no SMT) and Slurm 24.11.5 binds each of a node's four tasks
to one core unless told otherwise.

**The node's progress thread under Slurm's binding** (`mailbox`, 16 PEs):

| how the job ran | messages a second | the first PE's agent: running, waiting |
|---|---|---|
| bound, Slurm's default | 38,900 | 12%, 29% |
| bound; the agent alone given all four cores | 52,000 | 15%, 26% |
| bound; the agent asking for a 0.1 ms slice | 63,500 | 19%, 26% |
| bound; both | 67,500 | 20%, 24% |
| unbound, `--cpu-bind=none` | 243,000 to 249,000 | 68-71%, 1-2% |
| unbound, with the slice | 255,000 | 71%, 1% |

Bound, three cores sit near 13% while the thread that moves the node's
messages waits for the fourth; moving or hurrying it adds up to 70% to the
bound rate, and only unbinding every thread gives the sixfold.

**The gets** (the sort's pull, 67 M keys a round; the push's round 4.07 s):
5.61 s a round bound and 5.19 unbound at the default 256 KiB landing arena,
4.85 at 1 MiB, 4.26 at 4 MiB. The pull's `exchange_s` had held the round's
closing sync, which waits for every PE's sort: the three-to-five-fold gap
the first runs reported was mostly that (`7a14994` times it apart).

**The sort's digits**: at 11 bits the local sort takes 0.69 s a round where
16 took 2.80; a round 1.88 s against 4.08, 35.7 M keys a second against 16.4.

**`mix` with `waits`**, ten minutes at `6ac31fc`: 52 periods, `waits` on each
team six or seven times, `failures=0`.

**A start that hung**: the first `pull-widened` never got past `shmem_init`
on pi-001. systemd-logind's `RemoveIPC`, on by default here, deleted pi's
files in `/dev/shm` ten seconds after the measurement's last ssh session on
the node ended, while two of the node's PEs had not yet opened the control
segment by name. A probe on pi-002 showed the removal; the record's item 6
has the rest. Every later run held a session open on each node.

**Retransmission timeouts**: 4 across the nodes in the 35 minutes of
`mailbox`, pull and sort runs, none on pi-002; 8 in the ten minutes of
`mix`, five on pi-002. The nodes' `ip -s link` counters show no error, drop
or miss since the morning's boot, over a billion packets each way a node.

## The scan, the progress thread, and an hour of `mix` (2026-09-28, overnight)

At `8deb4ec` to `f561e2d`, 4 × 4, release; the record is
`docs/decisions/2026-09-28-overnight.md`.

**The scan pipelined**: a 1 MiB scan of longs, the slowest PE's median of 21,
every result checked - 139.3 ms before (136.5 again), 145.1 at D = 1, 83.5 at
D = 2, the default, 59.0 at D = 4 and 47.9 at D = 8, against a floor of about
36 ms, node 0's link carrying 4 MiB each way; a reduce beside it 30 ms
throughout (49.5 at D = 1).

**The progress thread under Slurm's binding**, now built into the library:

| a mailbox, 16 PEs, 60 s | messages a second |
|---|---|
| bound, the kernel's slice, the agent on its PE's core | 42,900 |
| bound, the agent widened to its node's cores | 51,300 |
| bound, the agent's 100 µs slice (the default) | 57,500 to 62,900 |
| bound, both | 58,500 |
| unbound, the default | 257,000 to 259,000 |

`osoe-bench`, three runs each way alternating, with and without the slice: a
64 KiB reduce 2,388 against 3,155 µs, alltoall 9% faster. The note that stood
here, "no difference in small operations", does not hold: the round-three
review repeated the matrix on 2026-09-28 (the section below) and found the
slice costs the barrier and small collectives, bound and unbound. Two runs each
way, widened and not, both with the slice: widened, every small collective
slower in both - an 8-byte reduce 223 against 200 µs, a broadcast 221
against 197, a barrier 191 against 178, a 64 KiB reduce 2,819 against 2,385
- so the widening is an option (`OSOE_AGENT_CPUS=node`), not the default.

**An hour of `mix`** at `5f99dd3`, Slurm's default binding, the deadlock
detector on at its default: 3,608 s, `failures=0`, 504 MB sent by the
library, and nothing declared deadlocked. A second hour at `f561e2d`, with the
check that finds a vanished node with data in flight: 3,603 s, `failures=0`,
605 MB sent, and nothing declared deadlocked or vanished.

## The round-three review's session (2026-09-28, morning)

Tree `f561e2d`, source-identical to `b0e0b53` (the sha256 of all 357 tracked
source files), `release`; four Pi 5, kernel 6.18.50, 16 KiB pages, Slurm
24.11.5, NICs with interrupt coalescing at 49 µs; 116 runs between 06:40 and
08:07, driven from `pi-login`. Raw outputs: `docs/design/evidence/pi-cluster/2026-09-28-review/`
holds 137 of the 138, every one the numbers below come from, with trailing
spaces stripped and the nodes' hardware addresses removed from
`01-node-facts.txt`; the 138th, `ip link`'s output, is those addresses
alone.

**Reproduced.** The README's smoke test in 6.8 to 7.3 s, `failures=0` in 37 runs
over five shapes. The floor: fastest get 1.098 to 1.100 of the echo's, put and
quiet 1.089 to 1.092, fetch-add 1.099 to 1.101 (three runs, spread at most
0.006), the stream 0.999. Put streams 112.1 MiB/s from 64 KiB to 4 MiB; a 1 MiB
get 85 to 86 (76 to 77% of that). A mailbox bound against unbound: 6.9×, and
4.0× with the slice. `OSOE_AGENT_CPUS=node`, bound and beside the slice, 7 to
18% slower on small collectives. Eighteen minutes of `mix` over two seeds, 90
periods, 159 GB sent, `failures=0`, nothing declared deadlocked or vanished,
`/dev/shm` empty. SOS at 4 × 2: 165 programs, 163 build, 161 pass
(`bigget -l 100` in 22.2 s; `unit/spam` takes 83.5 s and `unit/mt_membar` 50.6 s
against the runbook's 120 s); the four that do not are the four documented.

**Corrected.** A 16-PE barrier is 176 to 183 µs under Slurm's default binding
and 196 to 208 unbound (four runs each); small collectives cost 4 to 17% more
unbound. And the agent's 100 µs slice, both sides (4 × 4, 16 PEs, three
alternating runs each way, medians in µs; the mailbox one 60 s run a setting;
each cell gives slice 100 / slice 0, then what the slice does to the time, or to
the messages a second):

| | bound (Slurm's default), slice 100 / slice 0 | unbound (`--cpu-bind=none`), slice 100 / slice 0 |
|---|---|---|
| mailbox, messages a second | 63,942 / 37,383: 1.7× more with the slice | 256,248 / 250,092: 2% more |
| 64 KiB reduce | 2,388 / 3,130: 24% less time | 2,802 / 2,800: the same |
| 4 KiB alltoall | 2,541 / 2,818: 10% less time | 2,515 / 2,742: 8% less time |
| barrier | 180.9 / 167.4: 8% more time | 200.9 / 196.4: 2 to 4% more time |
| 8-byte reduce | 211.1 / 190.6: 11% more time | 223.6 / 202.3: 11% more time |
| 8-byte broadcast | 211.2 / 187.4: 13% more time | 220.2 / 197.5: 11% more time |

## Step D's merge point (2026-10-04)

Release builds at 4 × 4 under Slurm's default binding, `osoe-bench` three times
each way, alternating, driven from `pi-login`; kernel 6.18.50, interrupt
coalescing at 49 µs, TCP and generic segmentation offload off - as on
2026-09-28 - the nodes up since 2026-10-02. The record is
`docs/decisions/2026-10-04-step-d-merge-point.md`; the raw outputs are
`docs/design/evidence/pi-cluster/2026-10-04-step-d/`.

Step D changes no data path, and its brief asked that nothing move beyond
noise. D and the tree before it ran as they were before their last changes -
comments, diagnostics and tests, none on a data path. Medians in µs, the range
of the three runs' medians:

| | before D (`c51ba87`) | with D | the records' tree (`b0e0b53`) | with D, again |
|---|---|---|---|---|
| barrier | 175.6 to 182.6 | 175.6 to 175.7 | 175.7 to 177.1 | 175.4 to 175.7 |
| reduce, 8 B | 198.3 to 207.9 | 197.6 to 197.8 | 198.6 to 198.8 | 197.6 to 197.9 |
| broadcast, 8 B | 195.8 to 204.9 | 195.2 to 195.4 | 195.5 to 195.7 | 195.3 to 195.5 |
| fcollect, 8 B a PE | 247.9 to 266.7 | 262.3 to 266.6 | 262.1 to 270.9 | 264.6 to 267.3 |
| alltoall, 8 B a PE | 345.0 to 349.5 | 343.3 to 344.3 | 344.7 to 345.6 | 343.6 to 345.1 |
| reduce, 64 KiB | 2,389 to 2,398 | 2,387 to 2,388 | 2,387 to 2,389 | 2,387 to 2,388 |
| broadcast, 64 KiB | 1,875 to 1,882 | 1,873 to 1,876 | 1,874 to 1,888 | 1,872 to 1,879 |
| fcollect, 4 KiB a PE | 684.8 to 686.3 | 685.3 to 686.0 | 685.5 to 688.5 | 684.7 to 687.0 |
| alltoall, 4 KiB a PE | 2,563 to 2,601 | 2,542 to 2,644 | 2,542 to 2,630 | 2,562 to 2,589 |

With D and without it, every line's median of medians is within 1% of the
other's, but the 4 KiB alltoall's, 2%, inside its runs' spread; the first run
before D was the slowest of its three in the small collectives. The point-to-
point lines - put and quiet, get and fetch-add, 8 B to 1 MiB - are within 1%
too (the raw files).

Against the records of 2026-09-28 (above), the barrier is within its 176 to
183, and the 8-byte reduce and broadcast are 6 and 7% faster than that
morning's 211.1 and 211.2. So the tree measured then (`b0e0b53`,
source-identical to `f561e2d`) ran again, alternating with D's: the same
lines as D's, within 1% on every line. The code since the records moves
nothing; the cluster runs these collectives faster than it did that morning,
with the same kernel and NIC settings. The author put it down to the cluster's
rebuild - how its nodes are plugged into the switch can move these lines - and
accepted it: these runs are the baseline step E is measured against, run
alternately on the day.

## Step E (2026-10-04, evening)

Release builds at 4 × 4, `srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4`,
driven from `pi-login`, the nodes and settings as at step D's merge point:
step E's tree in `~/osoe` against step D (`main` at `158c13e`) under
`~/osoe/build/compare/before`, three runs a side in each of three sessions,
alternating. The probe, `e-band.c`, times on the world a barrier, a long sum
reduction, a broadcast and an inclusive sum scan from 8 bytes to 1 MiB - each
PE's median over 200 calls (50 from 64 KiB), each between two barriers, the
slowest PE's printed. The record is `docs/decisions/2026-10-04-team-scans.md`;
the raw outputs are `docs/design/evidence/pi-cluster/2026-10-04-step-e/`. The
sessions ran three rules for a large scan's pieces: a lane's 16 KiB at every
size, 4 KiB below 64 KiB, and the one kept - the scan's share of D, at least
8 KiB and at most 16 KiB. D is the median of its nine runs, E of the third
session's three; µs, the slowest PE's median:

| | D | E | |
|---|---|---|---|
| barrier | 204 | 208 | +2% |
| reduce, 8 B / 4 KiB / 16 KiB / 1 MiB | 227 / 394 / 1,050 / 29,342 | 230 / 410 / 1,064 / 29,310 | +1.5% / +4% / +1% / 0% |
| broadcast, 8 B / 4 KiB / 16 KiB / 1 MiB | 209 / 303 / 628 / 27,872 | 209 / 306 / 610 / 27,866 | none |
| scan, 8 B / 1 KiB (small) | 370 / 497 | 374 / 507 | +1% / +2% |
| scan, 2 KiB / 4 KiB | 479 / 614 | 636 / 722 | +158 / +108 µs |
| scan, 8 / 16 / 32 KiB | 859 / 1,408 / 2,551 | 849 / 1,405 / 2,388 | 0.99 / 1.00 / 0.94 |
| scan, 64 KiB / 1 MiB | 4,897 / 73,349 | 3,929 / 48,596 | 0.80 / 0.66 |

The reductions and broadcasts did not move beyond 4%: the probe calls each
just after a barrier, which releases the tree's root node a hop before its
children - the way a grant travels - so a child finds its grant waiting (round
six's runs, below, measured the grant's cost when a lead is late). The scans of 2 and 4 KiB pay it - one piece, which nothing
hides - and the author accepted that with his choice to size the pieces; from
8 KiB they are step D's or faster, and from 64 KiB much faster, a lane's 16
KiB pieces moving a quarter of the messages D's 4 KiB did. The first rule was
1.17 times D at 16 KiB, one piece where D kept two of 4 KiB in flight; the
second 1.10 times D at 8 KiB and 1.05 at 32, where fewer messages won.
`osoe-bench`, three runs a side each session: no line moved beyond its
run-to-run spread. `osoe-pacer --quick`, once a side each session: `failures=0`
on both trees, 6.8 to 7.0 s.

**An hour of `mix`** on E's third tree, 16 PEs at 4 × 4 under `--cpu-bind=none`:
`failures=0` in 3,609 s (`mix-hour-summary.txt`, its summary lines).

**The adversarial read's two probes** (`probes/`), on E as merged - its pieces
sized in elements - against D, three runs a side alternating, medians in µs.
`scan-odd`, an inclusive sum scan of longs either side of a multiple of D:
2,049, 3,001 and 4,095 longs take what 2,048, 3,000 and 4,094 do (1,409, 1,915
and 2,445 against 1,408, 1,889 and 2,456), where E's earlier sizing in bytes
cut a third piece from them; D pays its own fifth 4 KiB piece at 2,049 (1,560
against 1,427). `scan-skew`, a 2 KiB scan - one piece on both trees, so that
only the grant differs - timed past the entry of the member made late by a
millisecond:

| late | D | E | E - D |
|---|---|---|---|
| nobody | 501 | 620 | +119 |
| world rank 5, a member with children | 407 | 666 | +259 |
| the root | 227 | 455 | +228 |
| world rank 15, a leaf | 456 | 450 | -6 |

A collective's hop on the cluster, the message and its handling, is 115 to 135
µs (D's 227 µs for the root's two hops down, 407 for rank 5's three): one hop,
two, two and none, as `scan_grant_timing.py` has it. The band's +158 µs at 2
KiB is the first case - the members enter as a barrier releases them, a member
and its child on other nodes together.

**Step E's merge point** (`merge/`): `main` with both parts against D,
alternating, three runs a side, the same probe and `osoe-bench`. The session's
own medians put the barrier 12% and the 8-byte reduce 15% above D's, on lines
step E does not touch. Under `--cpu-bind=none` both are bimodal from job to
job on both trees - a run lands near 182 µs or near 207 for the barrier, near
202 or near 229 for the reduce, and within a mode the trees agree to a
microsecond - and this session drew D's faster mode twice and E's once. Over
all twelve runs a side of the four sessions: the barrier +1%, the 8-byte
reduce +2%, broadcasts within 1%, the large reductions of 2 and 4 KiB +11 and
+19 µs, scans as above (2 and 4 KiB +138 and +88 µs; 64 KiB 0.79 and 1 MiB
0.67 times D). `osoe-bench`, under Slurm's default binding, shows one mode in
every run of every session, D and E alike: the barrier 175 to 177 µs, the
8-byte reduce 197 to 198, the 8-byte broadcast 195.

**Round six's runs** (`review/`, for its scale-and-performance review), three
runs a side alternating, medians. `reduce-skew`, `scan-skew` with a 4 KiB
reduction - one large piece - timed past the late member's entry, E less D:
nobody late -8 µs at 4 × 4 and +23 at 4 × 1, where no intranode phase hides
anything, so that it is the barrier's order that hides the grant; the root
node's lead a millisecond late +278 µs at 4 × 4 and +43 at 4 × 1; a middle
lead at a fan-out of 2 +216; a PE of the root's node that does not lead it +2;
a leaf node's lead +18, and at 4 × 1 -10. The grant's round trip when a parent
enters late, then, and nothing much otherwise. `piece-edge`, E alone: a
reduction's or broadcast's piece holds 16,368 bytes, a lane's slot less its
header, so 32,768 bytes are three pieces where 32,736 are two; on gigabit the
extra piece costs 0.5 to 4% at 4 × 4 (1,888 against 1,878 µs for a reduction
of 32 KiB, 1,087 against 1,047 for a broadcast) and up to 11% at 4 × 1 (a 16
KiB broadcast, 623 against 562), where the development VM measured up to 2.4
times.

## The release (2026-10-05)

The runbook end to end on the tree to be tagged (`beae139`; parts A and B ran on
`81ba75a`, its code), copied to `~/osoe` and built there per §2, Slurm's
default binding unless a line says `--cpu-bind=none`; the record is
`docs/decisions/2026-10-05-release-point.md`, the raw outputs
`docs/design/evidence/pi-cluster/2026-10-05-release/`.

- **§1-§2**: the release build with no warning; the probe, all ok on every node;
  tier 1 on the head node 86 of 86, and tier 2 on a node, given all four cores,
  626 of 626.
- **§4**: the smoke test exits 0 on a block and a cyclic distribution.
- **§5, the floor**: a get's, a put and quiet's and a fetch-and-add's fastest
  round trips 1.098, 1.090 and 1.099 times an io_uring echo's between the same
  two processes, their medians 1.10, 1.09 and 1.10; the stream 0.999 of the
  floor's rate. Every pin holds with room: a fastest trip may be 3.0 times the
  floor's.
- **§6, `osoe-bench`** at 4 × 4: a put and quiet of 8 bytes 168.9 µs, a get
  168.3; a stream at 112.1 MiB/s from 64 KiB, gigabit's rate; the barrier 175.7
  µs, the 8-byte reduce 197.5 and broadcast 195.1, fcollect of 8 bytes a PE 264.3
  and alltoall 342.7 - step D's merge point's lines, as they were; and the 64 KiB
  reduce 2,374 µs and broadcast 1,834, 0.6 and 2% faster than D's, a piece now 16
  KiB whole. Unbound, the barrier 196.6 µs and the reduce 210.4.
- **§7, endings**: a global exit returns its status, 7, from every task; a dead
  PE ends the job, naming rank 5 on every PE.
- **§8, the corpus**: NPB IS verifies, classes A and B; PRK's three kernels
  validate; ISx sorts and verifies, weak and strong; of the 72 examples, 64 exit
  0 on 4 × 2 and the other eight as the runbook expects - `global_exit` 99,
  `nullput`, `just-a-shmalloc` and `shmem_2dheat` named, `bcast3`, `bcast4`,
  `reduce-max` and `reduce-sum` ended by name for their active sets; OSU's
  nineteen programs run whole, the five collectives at 4 × 1 and 4 × 4; ssca1
  finds its three alignments, scores 55 to 53, on both maps; the SOS suite at 4
  × 2, 160 clean and `bigget` clean alone at `-l 100`, the runbook's 161 of 165.
- **The README's commands**, on the head node - Raspberry Pi OS Trixie on a Pi
  5 - from a fresh copy: the quick start as printed, built in 72 s with no
  warning, the ring's four lines on one node and on two logical nodes, the probe;
  `cmake --install`; the ring built by the installed `oshcc` under `srun -K1
  --cpu-bind=none -N2 --ntasks-per-node=4`, its eight lines; the README's batch
  script, its secret exported, its sixteen; and `osoe-stat --summary` on every
  node of a running job through `srun --jobid=J --overlap -N4
  --ntasks-per-node=1`, a line a node with every PE's routine.
- **§6, `osoe-pacer`**, each run `failures=0`: the default set - its ten
  exercises now, each given the run's 300 s, so 3,014 s in all; the accumulating
  sort, the pull and the sort without `shmem_ptr`; mailbox, teams, atomics, rma,
  locks, memory and collectives, ten minutes each; waits and deprecated, five;
  `start_pes`; the teams' exhaustion; and the counted runs, whose coverage lists
  are kept.
- **An hour of `mix`**, `failures=0` in 3,607 s; **an hour of `threads`** in a debug build with
  the test hooks and an overfilled landing zone, `failures=0` in 3,600 s and no `OSOE_CHECK_QUIET:` line.

## v0.2 step 2: the tree's shape from the link (2026-10-06 and 07)

The trees of v0.2's second step, copied to `~/osoe` and built there as release
with no warning; the records are `docs/decisions/2026-10-06-tree-shape.md`
(Part 1) and `docs/decisions/2026-10-07-shape-data-path.md` (Part 2), the raw
outputs `docs/design/evidence/pi-cluster/2026-10-06-v0.2-step2/`.

- **The link, measured at the end of `shmem_init`** (`link/`, Part 1's
  measurement as built): ten starts at 4 × 1 and five at 4 × 4, read from
  `SHMEM_DEBUG`'s start-up lines. A hop of 86.8 to 94.0 µs, a bandwidth of 116.5
  to 120.1 MB/s, the same on every PE of a start; the message cost 0 to 21.0 µs
  from start to start; 27.1 to 27.6 ms of each start.
- **The chooser on the data path against 0.1.0's one tree** (`comparison/`,
  Part 2 before its scans' change): `link`, the chooser, against `fixed` at
  0.1.0's defaults (a fan-out of 4, two pieces in flight) on the same build, and
  the tag's own build, alternating three times: the e-band probe (a barrier,
  reductions, broadcasts and scans from 8 bytes to 1 MiB) at 4 × 4 and 4 × 1,
  OSU's reduce, broadcast and fcollect to 1 MiB at both, `osoe-bench` at 4 × 4;
  `analysis.txt` each line's median of three, its spread, and the ratios. A
  1 MiB broadcast 1.43 to 1.47 times faster than `fixed` (OSU 19.1 against
  27.4 ms at 4 × 4), a 1 MiB reduction 1.20 to 1.26 (24.3 against 29.3 ms),
  every size from 64 KiB up faster - 128 to 512 KiB 1.08 to 1.44 - and below
  that within 7% either way, the larger differences not repeating in the scans'
  runs below; `fixed` against the tag, the build against 0.1.0 at 0.1.0's shape,
  within the noise or 2% on 167 of 170 lines, the other three 2.3 to 4.4% slower
  (a 1 KiB broadcast and two small fcollects at 4 × 4); fcollect, which takes no
  shape, unchanged by it. **The scans
  at 4 × 4 1.5 to 2.0 times slower from 8 KiB up**: a scan kept the ceiling's
  tree, whose default had gone from 4 to 8.
- **The 1 to 16 KiB sweep** (`comparison/sweep-*`): OSU's reduce and broadcast
  at 4 × 1 at fixed shapes, fan-outs 2, 3 and 4 and windows 2 and 4. On four
  nodes the star - every fan-out of 3 or more - is the fastest at every size from
  4 bytes to 16 KiB, the tree of 2's second level costing about 185 µs at the
  smallest and still 160 µs at 16 KiB, and the window moving nothing below
  16 KiB; the chooser takes the star at all of them.
- **The scans' change** (`scans-1/`, `scans-2/`): at 0.1.0's fan-out with the
  capacity's four pieces in flight a scan at 4 × 4 was as fast as `fixed` from
  1 to 32 KiB, 1.30 times faster at 1 MiB and 6.6% slower at 64 KiB; with
  0.1.0's whole shape, as merged, every line of the probe - barrier,
  reductions, broadcasts and scans, both layouts - within the noise of `fixed`
  or faster, the large reductions and broadcasts as above.

## v0.2 step 3: Träff's reduce-scatter and allgather (2026-10-07)

The branch `circulant`, copied to `~/osoe` and built there as release with no
warning; the records are `docs/decisions/2026-10-07-circulant.md` (Part 1: the
model, the arithmetic, the cost) and `docs/decisions/2026-10-07-circulant-data-path.md`
(Part 2: the data path, and these measurements), the raw outputs
`docs/design/evidence/pi-cluster/2026-10-07-v0.2-step3/`. Each comparison is
`OSOE_COLLECTIVE_SHAPE=link` - a large reduction takes Träff's where the cost
model says it costs less than the best tree - against `tree`, step 2's trees
alone, on the same build, alternating three times: the e-band probe at 4 × 4
and 4 × 1, OSU's reduce to 1 MiB at both and to 4 MiB at 4 × 1, `osoe-bench`
at 4 × 4; `analysis.txt` each line's median of three, its spread and the ratio.

- **As merged** (`comparison/`, `5cbd47e`): every reduction size Träff's takes
  faster than the trees, every size below unchanged. At 1 MiB 1.36 times at
  4 × 1 (OSU, 16.6 against 22.6 ms; the probe 1.36) and 1.32 to 1.41 at 4 × 4
  (OSU 18.4 against 24.3 ms; the probe 16.3 against 22.9); 1.43 and 1.46 at 2
  and 4 MiB; 1.32 to 1.49 from 64 to 512 KiB at 4 × 1, 1.14 to 1.33 at 4 × 4 by
  OSU and 1.58 by the probe at 64 KiB; at 16 and 32 KiB, the smallest Träff's
  takes here, 1.04 to 1.05 and 1.05 to 1.26 by OSU, 1.24 and 1.31 to 1.49 by
  the probe. To 8 KiB both run the same tree, within 1% of each other but for
  OSU's smallest sizes at 4 × 4, which read 2 to 8% faster under `link` - the
  same code. In `osoe-bench`'s loop at 4 × 4, reductions back to back with no
  barrier between them, the 64 KiB median is the trees' (2.26 against
  2.24 ms) and the fastest call 1.17 times faster - under the default
  binding only, H21 found on 2026-10-12. Broadcasts and scans, which
  take the same shape under both, moved by up to 8% either way between
  rotations: the noise of a three-rotation comparison here.
- **At the capacity's window** (`comparison-window-4/`, `4b9767e`), as Part 2
  first built it, each round in one row of a lane with four pieces in flight:
  1.14 to 1.19 times faster at 1 MiB, against the model's 1.34. A window of
  eight took 9.4% off its 1 MiB (`experiment-1/`), and taking an arrived piece
  before sending the next nothing; each round taking the rows of the lane its
  rounds leave it, 16 pieces in flight on four nodes, made it 1.35 to 1.42
  (`experiment-2/`), as merged.
- **The model** (`shape_timing.py`): the chooser's closed forms put Träff's at
  1.34 times the trees' speed at 1 MiB on four nodes, and OSU measured 1.36 at
  4 × 1 as merged - though both closed forms run 16 to 18% under the measured
  times there, and neither saw the capacity's window hold Träff's back. The
  event simulation's trees are within 10% of OSU's from 16 KiB; its Träff's
  within 8% to 128 KiB and 11 to 22% optimistic from 256 KiB, so its 1.72 at
  1 MiB is too high: it takes every lead's link carrying both ways at once at
  the full rate, which the Pi's leads did not reach - one progress thread a
  node moving both directions the likely reason, not measured
  (measured on 2026-10-12, H21 below: not the reason).

## v0.2 step 4: progress helpers (2026-10-07 to 09)

The four nodes at a gigabit, each tree copied to `~/osoe` and built there as
release, every comparison alternating its trees, three runs a side unless a line
says otherwise; the probes ran under `--cpu-bind=none`, `osoe-bench` and the
mailbox under Slurm's default binding. The records are
`docs/decisions/2026-10-07-one-thread-profile.md` (Part 0, the one thread's
ladder), `docs/decisions/2026-10-08-owners.md` (Part 1),
`docs/decisions/2026-10-08-control-wire.md` (Part 2) and
`docs/decisions/2026-10-08-helpers.md` (Part 3); the raw outputs are
`docs/design/evidence/v0.2-step4/`, whose `README.md` says which directory holds
what.

- **The unsent limit: a fetch-add behind a 1 MiB put stream** (Part 0,
  `2026-10-07-pi-head-of-line/`; the record's "What a small message waits behind
  a stream, on the Pi"). On two nodes of two PEs each, PE 0 streams 1 MiB puts
  to the other node while PE 1 times fetch-adds to it over the same connection
  (`hol.c`), two or three runs a line. Alone a fetch-add took 171 to 173 µs.
  Behind the stream, with nothing limiting the kernel's unsent queue, it waited
  14.4 to 16.6 ms at the median on the tree before Part 0 (`3ee3357`) and 16.1
  to 22.2 with a put filling the room, the kernel queueing some 2.6 MB ahead of
  it. With the queue limited to 16 KiB (`TCP_NOTSENT_LOWAT`) it waited 3.3 to
  3.9 ms on both trees, the 99th percentile 3.9 to 4.4 and 4.7 to 5.0, and the
  same with the limit as built (16,384 bytes, from the 116.5 MB/s the leads
  measured; `cap-1/`). The stream ran at 112 MiB/s in every line. At the tip of
  the first three rungs (`compare-1/`) the wait was 3.9 ms, the 99th percentile
  6.65 to 6.92, and `osoe-bench` was within the noise of `3ee3357` but for the
  streams of the pairs at 4 × 4, which share the links more evenly.
- **The zero-copy send on the Pi's NIC** (Part 0, `2026-10-08-zero-copy/`; the
  record's "What the zero-copy send does, on the Pi and over loopback"). A 1 MiB
  put stream between two nodes for 20 s with each node's agent CPU read from
  `/proc` (`putstream-cpu.c`), and `osoe-bench`'s latency and atomic lines at 2
  × 1, the setting off and on, three rounds each, alternating. The stream ran at
  112.1 MiB/s in every case. As first built, with every send allowed by
  reference, 87 to 89% of all sends went by reference (the kernel copying 11 to
  14% of those after all) and the sending agent spent 0.267 to 0.271 CPU-seconds
  a GiB against 0.30 to 0.52 off, but `osoe-bench`'s 1 MiB lines did not finish
  in 25 minutes: with both of a connection's buffers lent a quiet waited about
  half a second at a time. So one buffer at most is lent: 28% of sends by
  reference, the agent 0.320 s a GiB, every `osoe-bench` line the copying
  send's. At a gigabit the agent is 8% busy either way, so nothing here argues
  for a default and the setting stays off.
- **Part 1 at no helpers** (`2026-10-08-owners/`; the record's "On the Pi"):
  `owners` at `5264aba` against `main` at `44f24bb`, alternating three times.
  Nothing moved: every remote line of `osoe-bench` at 4 × 4 and 4 × 1 within
  0.3% at the median but a 4 × 1 get of 8 bytes in its two modes, the stream
  112.1 MiB/s in all six runs, a fetch-add behind it 3,859 to 3,883 µs on the
  base and 3,845 to 3,874 on the branch.
- **The control wire: a barrier behind a 1 MiB put stream** (Part 2,
  `2026-10-08-control-wire/`; the record's "On the Pi"). The branch at `3d30d1c`
  and, after the first adversarial read's answers, at `17df5ac`, each against
  Part 1's tree (`95f3339`), no helpers: PEs 1 and 3, one on each node, sync a
  team of the two while PE 0 streams to PE 2 (`barrier-behind.c`). **2.5 to 2.6
  times faster at the median, 2.9 at the 99th percentile**, the medians of three
  runs: at `17df5ac` 1,663 to 1,665 µs where Part 1's were 4,139 to 4,415, the
  99th percentiles 2,216 to 2,224 where they were 6,089 to 6,388, the stream at
  112 MiB/s under both. A fetch-add, which rides the data wire, still waited
  3,853 to 3,888 µs, and the 1.7 ms left of the barrier's is the link's. Nothing
  else moved - `osoe-bench` within 0.3%, the stream 112.1 MiB/s in all twelve
  runs, the mailbox, start-up - and the barrier alone did not cost (165 to 174
  µs against 174 to 175).
- **Part 3 at no helpers** (`2026-10-08-helpers/`; the record's "On the Pi"):
  `7903b15` against `main` at `fc99503` (Parts 0 to 2), alternating three times.
  Nothing moved: every remote line of `osoe-bench` at 4 × 4 and 4 × 1 within
  0.3% at the median, the put stream 112.1 MiB/s, a barrier of one PE a node 172
  to 173 µs alone and 1,664 to 1,669 behind a 1 MiB put stream, a fetch-add
  behind it 3,863 to 3,879 µs, in both trees; the mailbox 34,288 to 40,482
  messages a second for `main` and 37,110 to 39,579 for the branch. At two
  helpers, once and as information: a barrier 165 µs alone and 1,664 behind the
  stream, a fetch-add 171 and 3,876, the 8-byte lines 167.6 to 167.8 µs, and the
  mailbox 46,570 to 48,101 messages a second, about a fifth above no helpers'
  three runs.
- **Question 4: a blocking get against a put** (the exploration for the author's
  decision, `docs/design/blocking-get-proposal.md`; `2026-10-09-question4/`).
  `get-pieces.c` times a put and quiet, a blocking get, and a nonblocking get
  and quiet between two PEs on two nodes: the tree of `7903b15`, three runs of
  21 repetitions a size, the medians against the put's. A blocking get took 0.91
  to 0.93 of a put and quiet's time at 16 KiB and 1.23 to 1.28 from 64 KiB up
  (at 1 MiB 11.55 to 11.57 ms against 9.09 to 9.10: 2.5 ms, about seventeen of
  its round trips), and a nonblocking get and quiet 0.89 to 0.91 at 16 KiB and
  1.00 to 1.05 from 64 KiB up.
- **The merge point against step 3's**, no helpers (the review of v0.2 step 4's
  merge point, `docs/reviews/archive/v0.2-step4-review.md`): `main` at `c897948`
  against `512e913`, alternating three times on 2026-10-09 with Part 3's script
  (`2026-10-08-helpers/pi-compare.sh`). A barrier behind a 1 MiB put stream took
  1,665 to 1,669 µs at the median where step 3's took 18,322 to 19,917, the 99th
  percentile 2,225 to 2,230 against 28,776 to 29,896: the control wire's
  purpose. A fetch-add behind the stream took 3,871 to 3,883 µs where it took
  17,155 to 19,931: the unsent limit's. The sending agent spent 0.29 to 0.31
  CPU-seconds a GiB against 0.38 to 0.40. Unchanged: the stream at 112.1 MiB/s,
  and every remote line of `osoe-bench` within 0.7%, its small latencies about a
  microsecond longer.
- **Helpers on the Pi** (the merge point's review of scale and performance,
  `docs/reviews/archive/v0.2-step4-scale-and-performance.md`, S4; its own runs
  in `docs/reviews/evidence/v0.2-step4-scale-s4-pi/`):
  `OSOE_PROGRESS_HELPERS=2` against none, the collective lines of `osoe-bench`
  at 4 × 4 and 4 × 1 in Part 3's run (`2026-10-08-helpers/pi-7903b15/`) and the
  merge point's, which agree to within 3%, and a short run of the review's own
  at 4 × 4, alternating, two rounds each. Under Slurm's default binding, where a
  lead's main thread, agent and helpers share one core, two helpers made a 64
  KiB broadcast 6 to 30% slower, a 4 KiB alltoall 20 to 35% slower, and a 64 KiB
  reduction take 15 to 37% less time. Under `--cpu-bind=none` the broadcast was
  8 to 9% slower (1,490 to 1,498 µs at none against 1,618 to 1,632) and the
  alltoall 2 to 3% (2,507 to 2,525 against 2,567 to 2,582), and the reduction no
  faster (1,434 to 1,445 against 1,412 to 1,419), since unbound it was already
  as fast. In the review's short run the 8-byte reduction and broadcast were
  10 to 16% faster under both, but its runs at no helpers were slower than any
  other day's; Part 3's run and the merge point's, three runs at none against
  one at two, give the 8-byte reduction about 5% faster at 4 × 4 under the
  binding and nothing at 4 × 1 (the merge point's adversarial read of its fixes,
  finding 6). Four nodes only.

## v0.2 after step 4's merge point (2026-10-10)

- **A blocking get's pieces together** (`docs/decisions/2026-10-10-blocking-gets.md`,
  "The measurements"; `docs/design/evidence/v0.2-step4/2026-10-10-blocking-gets/pi-blocking-gets/`):
  question 4's probe, `get-pieces.c`, on two nodes of one PE, 21 repetitions a
  size, the medians, `main` at `7a42c92` against the branch alternating three
  times. A blocking get of a mebibyte took 11,550 to 11,559 µs on `main` and
  9,148 to 9,154 on the branch - a fifth less, the nonblocking get's time - and
  from 64 KiB up 1.00 to 1.06 times a put and quiet where `main`'s took 1.22 to
  1.28; a get of one piece, 16 KiB, was as it was. The branch's tests of gets
  on one Pi node's logical nodes passed 57 of 57 once the internode test's count
  was corrected, 48 of 57 before.
- **The zero-copy send refused for want of locked memory**
  (`docs/decisions/2026-10-10-zero-copy-locked-memory.md`, "The verification";
  `docs/design/evidence/v0.2-step4/2026-10-10-zero-copy-locked-memory/`,
  `pi-zero-copy-refused/` and `pi-zero-copy-refused-e53c9e2/`): on the Pi's
  Linux 6.18, which charges rings to the limit too, the seven zero-copy tests
  passed at `e53c9e2`, after the change's second read, each node saying its
  refusal once - one 6.18 gives at submission - and a job of two logical nodes
  under `ulimit -l 0` was refused at start-up, naming the limit. The first
  build's run had failed one of the seven, by the test's own defect.

## Round nine and its changes (2026-10-10, night, to 2026-10-11)

- **The tag's Pi checks rehearsed on `main` at `3b45472`** (round nine's lead,
  overnight; `docs/reviews/archive/v0.2-before-part4-review.md`, "The Pi
  rehearsal"; its raw outputs in
  `docs/design/evidence/pi-cluster/2026-10-10-round-nine-rehearsal/`):
  0.1.0's release-point runbook in its four parts. The build without
  a warning, the probe, tier 1 101 of 101, the smoke on block and cyclic maps,
  the floor, the benchmark, the endings, the corpus as at 0.1.0, every pacer run
  `failures=0`, an hour of `mix` `failures=0` in 3,603 s and an hour of
  `threads` under the debug checks `failures=0` - and tier 2 on one node, as
  the runbook runs it (`srun -N1 -n1 -c4`, `ctest -j4`), failing four runs of
  four, 758 or 759 of 760: from Linux 6.14 io_uring charges every ring to one
  count of locked memory for the user on the host, and the step's soft limit
  was the shell's 8 MiB, its hard limit unlimited (portability P9-1).
- **Tier 2 on one node with `shmem_init` raising the soft limit to the hard
  one** (`docs/decisions/2026-10-11.md`), the step's limits as before (soft
  8 MiB, hard unlimited): no ring refused in twelve runs. Two start-up tests
  failed in six of the first nine - `shmem.leftovers.restart.other-size` and
  `shmem.start-up.lead-ends-in-join` - none of them the library's: the second's
  job ran forty times of forty outside `ctest`, twenty on `main` and twenty on
  the change, and a sampler caught a rank's shell in uninterruptible sleep in
  an NFSv4 open of its script under `/home` past the launcher's limit, while
  the 64- and 70-PE jobs, whole now where they were refused in a second,
  started beside it. With those jobs run alone (`RUN_SERIAL`) and the
  lead-ends test's limit at 30 s, three runs of three passed, 764 of 764, in
  246 to 298 s.

## H21: Träff's model and the 64 KiB reduction back to back, again (2026-10-12)

Step 3 left two things open (`docs/decisions/2026-10-07-circulant-data-path.md`):
the event simulation's Träff's 11 to 22% optimistic from 256 KiB, one progress
thread a node moving both directions named the likely reason, and `osoe-bench`'s
64 KiB reduction back to back at 4 × 4 no faster under Träff's than under the
trees. Step 3 ran both under Slurm's default binding, where a lead's main thread
and its progress thread share one core, and before step 4's helpers. Measured
again on `main` at `02a7944`, release, built with no warning: `link` against
`tree` under the default binding and under `--cpu-bind=none`, at no helpers and
at two (`OSOE_PROGRESS_HELPERS=2`), alternating three times - OSU's reduce from
64 KiB to 4 MiB at 4 × 1 and `osoe-bench` at 4 × 4. The raw outputs, the
scripts (`h21.sh`, `analyse-h21.py`) and `analysis.txt` - each line's median of
three, its spread, the ratio, and OSU's lines over the simulation's - are in
`docs/design/evidence/pi-cluster/2026-10-12-h21/`.

- **The model's optimism is not the threads.** Träff's measured time over the
  simulation's (`tools/modelcheck/shape_timing.py` at the Pi's link, the sender
  model) from 256 KiB to 4 MiB is 1.12 to 1.22 under all four: 1.13 to 1.22 at
  the default binding and no helpers, as step 3 found (1.11 to 1.22, the same
  method on its files), 1.12 to 1.21 unbound, 1.12 to 1.17 at two helpers bound
  or not. At 1 MiB two helpers take 16.6 ms to 16.0. Two helpers carry the two
  directions on threads of their own and unbinding gives the main thread and
  the agent a core each, and neither closes the gap; the trees' simulation is
  within 7% at the same sizes at no helpers (0.94 to 1.07). What is left is what
  only Träff's asks of a lead - its link carrying both ways at once, which the
  simulation takes at the full rate each way - or a cost a byte the model lacks.
  A stream both ways at once between two nodes would tell the two apart; it was
  not run. The chooser's closed forms, both 16 to 18% under the measured times
  at 1 MiB, keep their ratio.
- **The 64 KiB reduction back to back was the binding.** In `osoe-bench`'s loop
  at 4 × 4, Träff's 64 KiB median against the trees': under the default binding
  at no helpers 2.27 against 2.21 ms, 0.97 times as fast (step 3: 2.26 against
  2.24); unbound 1.45 against 2.41, 1.65 times; at two helpers 1.44 against 2.76
  bound, 1.91 times, and 1.43 against 2.45 unbound, 1.71. Bound at no helpers
  Träff's loses 0.8 ms a call - its fastest call, 1.49 ms, is the unbound
  median - while the trees' is 8% faster bound than unbound. Two helpers on the
  lead's core take Träff's loss back, as the merge point's review found (S4),
  but slow the trees' to 2.76 ms and the 64 KiB broadcast, the same code under
  both, from 1.51 to 1.54 ms to 1.89 to 2.15. At 4 × 1, one PE a node, OSU's
  reduce shows no such loss: 1.39 ms at 64 KiB bound, 1.37 unbound. A site
  that binds each PE to its core (B11, H26) is where it matters.

## H29 and H22: the link both ways, and F1's control (2026-10-28 and 29)

`main` at `d24c54e9` on the head node; evidence
`docs/design/evidence/pi-cluster/2026-10-28-h22-h29/`; the records
`docs/decisions/2026-10-28.md` and `docs/decisions/2026-10-29.md`.

**H29.** A stream both ways at once against one way, two pairs of nodes, three
rounds: bare TCP 112.0 to 112.3 MiB/s either way, osoe's 1 MiB puts 112.1 one
way and 110 to 111 each way at once. The link carries both ways at the full
rate, so H21's Träff's, 12 to 22% slower than its simulation, pays a cost the
model lacks, not the link's.

**H22, F1's control.** F1's trigger map (`h22.sh`: `shmem_fcollectmem` over
the world, 200 calls after 20, a call over 20 ms slow) at 4 × 1, 4 × 2 and
4 × 4 over F1's block sizes, twice, and with rank 0 on `pi-002`:

| | offload off, 1.4 h after boot | offload on, 13 h after boot | offload off again, 23.7 h after boot |
|---|---|---|---|
| slow calls | none at any size; the slowest 3.9 ms | 1 to 10 in 200 at 12 of the 38 points run - 32 to 49 KiB at 4 × 1, 16 and 20 KiB at 4 × 2, 28 and 32 KiB with rank 0 on `pi-002`, none at 4 × 4 - each 44 to 45 ms | none at any size; the slowest 4.3 ms |
| TCP retransmission timeouts | none | none | none |

Offload on brings stalls back where F1 found them, but not F1's: 45 ms
waits with no retransmission timeout, where F1's were 0.5 to 36 s, the timeout
doubling. The nodes were thirteen hours up where the baseline's were not two,
so the offload went off again, the nodes then nearly a day up: nothing
stalled. **The stalls follow the offload, not the uptime**, and H22 is closed.
That settles the confound "F1, investigated" left - its clean runs came
minutes after every node rebooted - but not F1: why the offload stalls calls at
these sizes, and why for seconds in September and 45 ms now, is not known.

