# T46's evidence (2026-10-20): the one-second tail at 128 logical nodes on tier 3

The item (`docs/prompts/v0.2-open-items.md`, T46; the author's decision in
`docs/decisions/2026-10-19.md`, "For the author" item 4): at 128 logical nodes a few leads
connect their mesh a second late in some starts (H23's `timing/alternating.txt`), "likely a
dropped packet's retransmission" - find it by counting drops around a run. A measurement: no
library code, test or `tier3.sh` is changed.

## What it found

**The late leads are lost SYN and SYN-ACK frames, dropped by the kernel's per-CPU backlog queue
while tier 3's bridge floods ARP. The second is TCP's own initial timeout, which osoe's dial
deadline matches.** At 128 nodes the bridge turns each of a run's 8,100 ARP broadcasts into 127
frames: the nodes receive 1.46 million frames for the 0.38 million they send, and the queue behind
`net.core.netdev_max_backlog` (1,000 a CPU) overflows on the bridge-to-node hop - 90 to 1,600
frames a run (`softnet_stat`'s `dropped`). In the eight captured runs 17 of the 4,654 frames dropped
were a SYN or a SYN-ACK of the start-up; each of those connections waited a second. With the queue
raised to 65,536, six runs lost no frame and none had a late lead; at the default all six lost frames
and four had late leads. A lost ARP frame is not it. Confidence: high that a lost handshake frame is
the mechanism, medium on what fills the queue (below).

## What ran

The development VM `osoe` (aarch64, 4 CPUs, Linux 6.12.95, HZ 250), alone on it under
`/tmp/osoe-vm.lock`, on the night of 2026-10-19 (the VM's clock 23:23 to 23:30), the library of
`main` at `61f36585` built with the `tier3` preset (0 warnings). 42 jobs of `osoe-bench barrier
quick`, one PE a node, 8 MiB symmetric heap, `SHMEM_DEBUG=1` (its "connected its mesh in" lines name
the late leads), and two idle windows, each in a fresh `tier3.sh` of 128 namespaces on the Pi-like link
(100 us, 1 Gbit/s) unless said. `t46-plan.sh` ran them, `t46-series.sh` a series, `t46-wrapper.sh` as
tier 3's launcher. Around each job `t46-snapshot.py` read every namespace's `/proc/net/snmp` and
`/proc/net/netstat` (the counters `nstat` prints), `link0`'s line of `/proc/net/dev`, the host's
`softnet_stat` and `arp_cache`, the bridge's ports and the CPUs' times; in the captured series
`t46-sniff.py` kept, in each namespace, the ARP frames the node sent or was asked and every TCP frame with
SYN or RST set, sent or received, with kernel timestamps (a filter in the kernel, checked against made-up
frames by `--selftest`; no capture lost a frame).

| series | runs | what |
|---|---|---|
| `base` | 10 | the tail and its counters |
| `capture` | 8 | the same, with the packets captured |
| `ab-default`, `ab-backlog` | 6 + 6 | alternating, `netdev_max_backlog` at 1,000 and at 65,536 |
| `static` | 6 | a permanent neighbour entry for every other node: no ARP is sent |
| `lossy` | 3 | control: tier 3's `lossy` link, netem dropping 0.1% of the packets |
| `portloss` | 2 | control for the capture: 2% of the frames to nodes 5 and 70 lost on their bridge ports, captured |
| `null` | 2 | no job, 4 s of idle namespaces: the counters by themselves |
| `smoke` | 1 | 16 nodes, captured: the harness against the real binaries |

The kernel's settings were raised for the runs and put back, as the plan's last line and a check
afterwards show: `gc_thresh2` and `gc_thresh3` to 32,768 and 65,536 (found 512 and 1,024; `tier3.sh`
refuses 128 nodes otherwise), `gc_thresh1` (128) untouched, `netdev_max_backlog` 1,000 except 65,536
in `ab-backlog`. The kernel log gained no line during any job (`dmesg.txt`, empty in all, not kept).

## The tail

Of the 30 runs at the default backlog (`base`, `capture`, `ab-default`, `static`) 12 had late leads:
45 leads, 410 to 968 ms behind their run's median, at 1.0 to 1.4 s where the median lead took 0.25 to
0.7 s. Nothing lies between: of the 4,496 leads of the 36 runs that lost no frame on purpose (not
counting the slow first run), 4,451 were within 190 ms of their run's median and 45 were 410 ms or more
behind it. "Late" is therefore 0.4 s behind the median. `base/run-01` was slow throughout - every lead
2.2 to 2.4 s, no drop - and is counted, but is not a tail. `series-summary.txt`:

| series | runs with late leads | late leads | runs that dropped frames | frames dropped | slowest lead, ms (least / median / most of the runs) |
|---|---|---|---|---|---|
| base | 4 of 10 | 13 | 9 | 5,145 | 509 / 1,052 / 2,430 |
| capture | 3 of 8 | 14 | 8 | 4,654 | 559 / 647 / 1,354 |
| ab-default | 4 of 6 | 15 | 6 | 5,633 | 480 / 1,194 / 1,420 |
| ab-backlog | 0 of 6 | 0 | 0 | 0 | 424 / 465 / 518 |
| static | 1 of 6 | 3 | 6 | 2,862 | 259 / 290 / 1,078 |
| lossy | 3 of 3 | 162 | 3 | 4,420 | 1,500 / 1,513 / 1,642 |
| portloss | 2 of 2 | 27 | 2 | 5,724 | 1,400 / 1,505 / 1,505 |

## What moved with the late leads

All of it is in `reports/counters-*.txt`. The nodes of the 30 runs at the default backlog, 45 late and
3,795 on time (`counters-nodes-at-default-backlog.txt`):

- **Drops, on one hop.** The frames dropped are the bridge ports' `tx_dropped` (5,145 of 5,145 in `base`,
  the same count as `softnet_stat`'s) and `link0`'s `rx_dropped` (5,076): the hop from the bridge into a
  node's namespace. `link0`'s `tx_dropped` and the ports' `rx_dropped` - the hop out of a node - are 0.
  A CPU has its own queue: a run's drops fall on two to four CPUs, a median 87% on the busiest.
  A late lead had a frame dropped on its way to it in 42 of 45 cases - and so had 3,345 of the 3,795
  leads on time, most drops being copies of a broadcast nobody needed. The count tells that frames
  are lost, not which.
- **TCP did not retransmit a SYN.** `TcpExtTCPSynRetrans` and `TcpExtTCPTimeouts` moved in one late lead
  and in no other node of the 30 runs; `ListenOverflows` and `ListenDrops` in none of the 44. In the 36
  runs that lost nothing on purpose, `TcpExtTCPFastRetrans` moved in 8 (1 to 5 segments) and
  `TcpExtTCPOFOQueue` in 9. `TcpRetransSegs` moves in every run (1,077 to 4,144) and is tail loss probes: in
  35 of the 36 it is exactly `TcpExtTCPLossProbes` plus the fast and SYN retransmits (the 36th, 4 fewer).
  It says nothing about loss.
- **ARP is quiet.** `res_failed` and `unresolved_discards` were 0 in all 44 runs, `table_fulls` and
  `forced_gc_runs` in all 42 with a report.

## The packets

The 8 `capture` runs (`reports/packets-stories.txt`, `packets-totals.txt`): 801,587 events; 65,094 ARP
requests, 2 of them sent and never received; 65,092 replies, none lost; 135,286 SYNs, 4 lost; 131,093
SYN-ACKs, 13 lost. No ARP pair took 200 ms to resolve. 17 connections took 200 ms or more from their
first SYN or never answered, and each is a SYN or SYN-ACK sent and never received (the first SYN lost 4
times, the first SYN-ACK 13). The 14 late leads of the three runs that had any - 6, 4 and 4 - are each
the caller or the answerer of one.

The SYN is never sent again with its sequence number (0 of 135,286). The caller closes the socket and
dials again **from a new port 999.4 to 1,010.1 ms after the first SYN** (17 times, median 1,000.4) -
`PROBE_NS`, 1 s, in `shmem/src/listener.c` - and the new connection is answered in 0.14 to 0.18 ms. Where
a SYN-ACK was lost, the answerer's kernel sent it again 1.023 s after the first and met a closed socket,
and the caller reset it (`capture/run-01`, nodes 51 and 62): that is the one late lead whose namespace
shows `TcpExtTCPSynRetrans`. The lost frames are in bursts - six SYN-ACKs to nodes 8 and 51 within a
microsecond at 231 ms - and 16 of the 17 are 93 to 439 ms after a run's first frame, in the span (0.67 to 0.71 s)
in which its ARP requests were sent. The 17th, in `capture/run-02`, was in the start-up before the
mesh: a SYN-ACK to node 17 at 3.3 ms, a new dial 1,010 ms later, and the mesh did not begin for a second
- a delay no "connected its mesh in" line shows, as the leads time the mesh from the rendezvous's release.

## The controls

- **Loss on purpose, seen by the counters** (`lossy`): netem dropped 422, 407 and 437 packets a run, and
  `TcpExtTCPFastRetrans` was 46, 41 and 32 (0 to 5 in the 36 lossless runs), `TcpExtTCPOFOQueue` 137, 41
  and 32 (0 to 23), `TcpExtTCPTimeouts` 6, 2 and 4 and `TcpExtTCPSynRetrans` 5, 2 and 2 (0 in all but one
  lossless run, where 2), with 51 to 60 late leads a run, and the job took 21 to 28 s against 1.5 to 3.3.
  So the counters see loss, and their silence in the lossless runs is a finding: no data segment was
  lost, and no SYN reached the kernel's timer, osoe's deadline coming first.
- **Loss on purpose, seen by the capture** (`portloss`, 2% on the ports to nodes 5 and 70; netem dropped
  298 and 246 frames, then 307 and 231): 87 ARP requests, 36 ARP replies, 12 SYNs and 11 SYN-ACKs lost
  in the two runs, as the report counts them; 19 dials made again 999.9 to 1,001.9 ms later; 25 of the
  27 late leads accounted for. A lost ARP request cost 250 ms in 36 pairs, the target's next
  broadcast completing the entry (the kernel takes a sender's address from any ARP frame); in 3 more - a
  lost request and two lost replies, with no such broadcast - the neighbour's one-second retry, 1.01 to
  1.03 s. So a lost ARP frame can cost a second; in the 8 runs without injected loss two requests were
  lost and neither did.
- **The backlog** (`ab-default`, `ab-backlog`, six pairs alternating): frames dropped 90, 1,404, 1,411,
  1,500, 865 and 363 against none in six; late leads 8, 0, 0, 2, 3 and 2 against none. Four of six
  against none is p = 0.03 by itself (Fisher, one-sided); with the packets the mechanism does not rest on
  it. The mesh medians did not change (387 to 453 ms against 396 to 484).
- **No ARP** (`static`): the mesh takes 250 ms rather than 450, and the nodes receive 0.41 million frames
  for the 0.37 million they send rather than 1.46 for 0.38 - yet 191 to 934 frames are dropped a run,
  and one run in six has a tail (3 leads at 1.05 to 1.08 s).
- **Nothing running** (`null`, 4 s, 128 idle namespaces): the namespaces sent 112 and 98 frames and
  received 17,068 and 15,268, the bridge flooding their IPv6 multicast (router and neighbour
  solicitations, MLDv2 reports, as the 24-node check below shows); 0 frames dropped in one window and 142
  in the other.
- **The snapshot against `nstat`** (`nstat-check/`, 24 nodes, in the `osoe-load` VM): `nstat -asz` in
  node 0's namespace printed 383 counters; every IPv4 and TCP counter equals the snapshot's. The 17
  that differ are IPv6, from `/proc/net/snmp6`, which the snapshot does not read.

## What it says about osoe, for the author

Nothing here is a defect to fix in the library, and nothing was changed.

- The second is TCP's. A lost SYN costs the initial retransmission timeout, 1 s (RFC 6298), on any
  stack, and `PROBE_NS` is also 1 s: osoe's deadline and the kernel's timer coincide, and osoe's fires
  first (1,000 ms; the answerer's SYN-ACK timer, where it is seen, 1,023). By this reading a longer deadline
  would not lengthen the tail, and a shorter one for a told port would shorten it on a lossy path at the
  price of dials piling up on a slow one: a library decision, neither taken nor tried.
- The loss is tier 3's. On a real network a handshake frame is lost at the network's own rate; here one
  kernel's bridge floods 127 frames for each ARP request and overflows a queue of 1,000. A rehearsal this
  size can raise `net.core.netdev_max_backlog` in its own harness, as it raises `gc_thresh3` (65,536
  held six runs without a drop); `tier3.sh` is unchanged. That also hides, in tier 3, what a lossy path
  does to a start: one second for a lost handshake frame, anywhere in it.

## What is not shown

- The drop site is read from `softnet_stat` and the devices' drop columns, not traced.
- Six pairs are few; the packets and the counters carry the conclusion, the pairs confirm it.
- What fills the queue: ARP copies are 1.09 of the 1.46 million frames the nodes receive, but the `static`
  runs (no ARP) and the idle windows also dropped frames, so the IPv6 multicast of 128 namespaces and the
  SYN bursts overflow it too. Not separated (IPv6 off in the namespaces would).
- `base/run-01` is unexplained: no drop, 124 `time_squeeze`, every lead 2.2 to 2.4 s - the host, probably.
- The captures of 8 of the 11 captured runs are not kept (1.2 MB each): `reports/packets-stories.txt` is
  the tool's output over all of them, and `runs/capture/run-01`, `run-03` and `runs/portloss/run-01`
  reproduce their rows (`t46-capture-report.py --totals`).

## Files

`t46-plan.sh`, `t46-series.sh`, `t46-wrapper.sh`, `t46-snapshot.py` and `t46-sniff.py` ran the plan
(`flock -w 3600 /tmp/osoe-vm.lock bash t46-plan.sh TREE ~/t46-out 128`, TREE a `git archive` built with the
tier3 preset); `t46-report.py` and `t46-capture-report.py` read it; `t46-collect.sh` made this directory
from the plan's output. `environment.txt` and `progress.txt` are the plan's; `series-summary.txt`;
`reports/` (`counters-SERIES.txt` for each series, the two node tables, `packets-*.txt`);
`runs/SERIES/run-NN/` (`before.json.gz` and `after.json.gz`, the counters either side of the job;
`err.txt.gz`, the job's standard error; `job.txt`; `capture.txt.xz` in three) and `runs/logs/`.
The lines in `runs/*/series.txt` and `runs/logs/` are as the plan printed them, with the report's first
version (late = 0.8 s behind, and a TCP-opens column that proved noisy); `reports/` are the final.
