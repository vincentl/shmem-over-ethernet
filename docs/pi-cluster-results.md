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
