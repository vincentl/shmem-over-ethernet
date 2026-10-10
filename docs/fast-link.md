# Running osoe on a fast link: v0.2 step 4's Part 4

The runbook for the measurements that wait for a link faster than the Pi
cluster's gigabit: v0.2 step 4's Part 4
(`docs/prompts/done/v0.2-step4-progress-helpers-prompt.md`, "Part 4: gated on a
fast link"), and the questions earlier parts left to it. Written before any such
machine exists, so that one is not paid for while scripts are written: every
script below has run end to end on the development VM's logical nodes
(`KIT_LAUNCHER=osoe-launch`, `docs/fast-link/dry-run.sh`; its run of 2026-10-10
is `docs/design/evidence/v0.2-step4/2026-10-10-fast-link-dry-run/`), and none yet
on a cluster - `slurm-setup.sh`, and `build.sh`'s copying to other nodes, not
at all.

What Part 4 decides, and from what:

| Question | Where it was left | What decides it here |
|---|---|---|
| Whether a node should run progress helpers by default, and how many | the progress helpers proposal's §8 item 3: no helpers by default, a count from `OSOE_PROGRESS_HELPERS` alone, until a fast link has been measured (`docs/decisions/2026-10-07.md`, items 4 to 6) | §5: the put and get streams and the mailbox at 0, 1, 2 and 4 helpers and 8 to 32 PEs a node, against the bare stream (§4) |
| The zero-copy send's default, and whether a connection needs a third buffer with it on | `OSOE_WIRE_ZERO_COPY`'s row: "Not timed on a fast link" | §7 |
| The ladder's rungs on a fast link: the unsent limit and its rule, the connection buffer, the receive buffers | Part 0's ladder, measured on the Pi and the VM (`docs/decisions/2026-10-07.md`, item 12) | §6 |
| The landing zone's size, which holds a large get | `docs/design/blocking-get-proposal.md`, (d) | §8 |
| The receive pool at many peers sending at once | the review of step 4's merge point, scale S3 | §9 |

And beside them, osoe's first x86-64 kernel and its first node counts past the
Pi's four (§10).

Every number this produces is evidence for a decision the author makes; none
changes a default by itself. The README's sentences each section replaces are
listed in §11.

## 0. The machines

**Five nodes at least, eight better.** A node's progress helpers divide its
*peers*, not its streams: the data wire toward peer slot p is helper
`1 + p mod H`'s (`owner-map.h`; the README's `OSOE_PROGRESS_HELPERS` row), so a
pair of nodes keeps one helper busy whatever H is, and four helpers need four
peers - five nodes - to all have work. Eight nodes give each node seven peers,
which four helpers share two, two, two and one. The brief's "an x86-64 pair"
answers the zero-copy, ladder and landing-zone questions (§6 to §8) but not the
helpers' (§5).

**What each node needs:**

- x86-64, a link above a gigabit, and Linux 6.7 or newer with liburing 2.6 or
  newer: Debian 13 (liburing 2.9), Ubuntu 25.10 (2.11) or 26.04 LTS (2.14) as
  shipped; Debian 12 (2.3) and Ubuntu 24.04 (2.5) do not configure
  (`CMakeLists.txt`'s `liburing>=2.6`).
- Eight cores or more: the sweep runs up to 32 PEs a node, and 8 to 16 is where
  the cores and the helpers compete.
- The nodes on one network, as close as the provider places them.
- Every node able to reach every other's address on the ports osoe listens on
  (`OSOE_PORT_RANGE`, 17000 to 26999 by default) and the run's own (§4: 18000
  to 18063 for `echo-uring`, 5201 for `iperf3`).

**On Google Cloud** - the costing of 2026-10-09 (`docs/decisions/2026-10-09.md`,
item 3): `c4-standard-16` (16 vCPUs, eight cores, up to 23 Gbit/s), the
`debian-13` image, the gVNIC interface the C4 series requires, and a compact
placement policy, at about $0.79 a node-hour on demand. Eight nodes for four
sessions of four hours is about $100. The account is the author's, and so is
every launch. Note the VPC's MTU when the network is made (1460 by default,
up to 8896); §1's facts record it, and a change between sessions makes their
numbers incomparable.

**Sessions.** Each section says how long it takes; in four-hour sessions:

1. Setup, facts, the suite on a node, the floor and the bare stream (§1 to §4).
2. The helper sweep (§5), about an hour at eight nodes, with time to repeat
   what it shows.
3. The ladder, the zero-copy send, the landing zone, the fan-in (§6 to §9).
4. The cluster at size (§10), and whatever the first three left to repeat.

Bring everything back at the end of each session (§11): the machines are
deleted between sessions, and nothing on them survives.

## 1. Setup

The kit is this file's directory, `docs/fast-link/`. On the first node, which
also runs Slurm's controller, as a user with `sudo`:

```sh
sudo apt-get install -y build-essential cmake pkg-config liburing-dev git rsync \
    slurm-wlm munge ethtool iperf3
```

and the same on every other node (the list `NODES`, one name a line, every
node's name as the others resolve it, the first node first):

```sh
for n in $(tail -n +2 NODES); do ssh $n sudo apt-get install -y build-essential cmake \
    pkg-config liburing-dev rsync slurm-wlm munge ethtool iperf3; done
```

Then, on the first node:

```sh
docs/fast-link/slurm-setup.sh NODES       # munge's key, slurm.conf, the daemons, on every node
docs/fast-link/build.sh NODES             # the tree to every node at the same path, built there
```

`slurm-setup.sh` writes one partition of every node with its cores and memory
as `slurmd -C` reports them, `TaskPlugin=task/affinity`, and
`PropagateResourceLimits=NONE`, so that a step takes the node's limits rather
than the login shell's. It also sets, on every node:

- `RemoveIPC=no` for systemd-logind: with the default, a user's files in
  `/dev/shm` go ten seconds after the user's last session on a node ends, and a
  job still in `shmem_init` then waits for ever (`docs/pi-cluster.md`, §1);
- an unlimited locked-memory limit (`memlock`) for the user: the zero-copy send
  charges every page it lends to it until the notification, and its default of
  8 MiB is shared by the user's processes on a host (§7);

Read what it prints. `build.sh` copies the tree with `rsync` to the same path on
every node, builds the release preset on each in parallel, and compiles the
kit's probes beside it, into `build/release/kit/`: everything a step runs is at
the same path on every node, without a shared file system.

## 2. The facts

```sh
docs/fast-link/facts.sh NODES out/facts
```

On every node: the kernel, the CPU (`lscpu`), memory and `/dev/shm`, the page
size, the governor, the interface's driver, speed, MTU, offloads, interrupt
coalescing and queues (`ethtool -i/-k/-c/-l`), the TCP and core network
settings (`net.core.netdev_max_backlog`, `rmem_max`, `wmem_max`,
`net.ipv4.tcp_rmem`, `tcp_wmem`, `tcp_congestion_control`), the locked-memory
limit a Slurm step sees, `io_uring_disabled`, logind's `RemoveIPC`, liburing's
version, each node's name as every other resolves it, and Slurm's
configuration. Kept with the results: a number without them is not comparable
with another.

## 3. The suite on a node, the probe on every node

```sh
srun -N$(wc -l < NODES) --ntasks-per-node=1 build/release/tools/probe/probe
srun -N1 -n1 -c16 ctest --test-dir build/release -L tier2 -E '^model\.' -j8 --timeout 300 > out/suite.txt
```

The probe must say `ok` for every facility on every node. The suite is the
release build's tier 2 on one node's logical nodes; tier 1 compiles, and runs on
the node that built it (`ctest -L tier1`). The floor pins (`shmem.floor`,
`echo-uring`'s) were set on the VM and may fail here: a finding to keep, not a
reason to stop. About twenty minutes.

## 4. The floor and the bare stream

```sh
docs/fast-link/floor.sh NODES out/floor
```

Between the first two nodes: `shmem_test_floor`, one PE a node - the library's
get, put and quiet, fetch-add and stream against an io_uring echo and stream
between the same two processes (design §8.4); `echo-uring` from one node to the
other, one connection; then 1, 2, 4, 8 and 16 `echo-uring` connections at once,
each its own pair of processes, their rates summed - the bare aggregate a node
with several connections can reach, which one connection's rate may not show
where a provider limits each flow; and `iperf3` with the same counts of streams,
as a cross-check (design §8.4: "a useful cross-check where it is installed").
About fifteen minutes.

## 5. The progress helpers

```sh
docs/fast-link/helpers.sh NODES out/helpers          # three rounds, 0 1 2 4 helpers, 8 16 32 PEs a node
docs/fast-link/summarize.py out/helpers
```

Each case is a fresh job at one helper count (the count shapes the node's
connection area and is agreed at start-up), every node at once:

- `multistream put 1048576 10`: every PE puts 1 MiB blocks to the PEs of every
  other node in turn for ten seconds, a quiet every 8 MiB; the job's and each
  node's rate, and the CPU each node's progress threads - the lead's agent and
  its helpers, by their names `osoe-agent` and `osoe-helper-N` - and its PEs'
  own threads spent, a GiB moved;
- `multistream put 65536 10`, the same in 64 KiB blocks: more messages a byte;
- `multistream get 1048576 10`: the same with nonblocking gets, a quiet every
  8 MiB;
- `osoe-pacer --seconds 20 mailbox`: small messages with signals to PEs on every
  node, messages a second.

The order of the helper counts changes each round, so that a drift in the
machines does not read as a helper count's. Three rounds of 4 helper counts × 3
PE counts × 4 workloads are 144 jobs of 10 to 20 seconds and their start-ups:
about an hour at eight nodes. `ROUNDS`, `HELPERS`, `PES_PER_NODE` and
`WORKLOADS` in the environment shorten it.

`summarize.py` prints, for each workload, PE count and helper count, the median
and range of the three rounds' rates and of the progress threads' CPU a GiB, and
beside them §4's bare aggregate at the matching connection count. What decides
the question (§8 item 3) is whether a helper count raises the rate a node
reaches toward its bare aggregate, at what CPU, and whether that holds at every
PE count - a rule the author may then give, for example a count that follows
the link's measured bandwidth and the node's peers, or the setting alone as
today.

## 6. The ladder

```sh
docs/fast-link/ladder.sh NODES out/ladder
```

Part 0's rungs that a setting switches, each against the default, alternating,
three rounds, between the first two nodes:

- **The unsent limit** (`OSOE_WIRE_UNSENT_BYTES`): the default - the rule's
  limit from the measured link, which `SHMEM_DEBUG` reports and the script
  records - against 0 (the system's), 64 KiB, 256 KiB, 1 MiB and 4 MiB. Each
  with `hol` (a fetch-add's round trip behind a 1 MiB put stream on the same
  connection, four PEs, two a node) and the stream's rate: the limit trades the
  one for the other, and its rule's 128 µs of turnaround is a guess no one has
  timed on a fast link (the scale review, S5).
- **The connection buffer** (`OSOE_AM_BUFFER`): 128 KiB, 256 KiB (the default),
  1 MiB and 4 MiB, with the stream and `hol`.
- **The receive buffers** (`OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS`): 16 KiB × 64
  (the default), 64 KiB × 64, 16 KiB × 256, with the stream.

Rungs 1 (fill the room) and 4 (parse in place) are code, not settings: measuring
them without means building the trees before them (`3ee3357`, `de3dc42`,
`49c62d3`, `2dcf8d6`, `44f24bb`), which this runbook leaves out unless the
others leave time. 33 jobs of about fifteen seconds: about ten minutes.

## 7. The zero-copy send

```sh
docs/fast-link/zero-copy.sh NODES out/zero-copy
```

`multistream put` at 1 MiB and 64 KiB, with `OSOE_WIRE_ZERO_COPY` off and on, and
on at a threshold (`OSOE_WIRE_ZERO_COPY_BYTES`) of 16 KiB, 64 KiB and 256 KiB; at
no helpers and at the count §5 found best; with the rate, the progress threads'
CPU a GiB - which is what the zero-copy send exists to save - and the node's
counters of sends by reference, notifications and sends the kernel copied after
all (`osoe-stat`). It records the locked-memory limit each step saw: below
what the lent pages need, the sends by reference are refused and the
connections copy, which the notice "refused for want of locked memory" says;
a run that shows it measured the copy. 24 jobs at one helper count, 48 at two:
ten to fifteen minutes.

## 8. Gets, and the landing zone

```sh
docs/fast-link/gets.sh NODES out/gets
```

Question 4's probe (`get-pieces`), one PE on each of two nodes: a put and
quiet, a blocking get, and a nonblocking get and quiet, from 16 KiB to 64 MiB,
at a landing zone (`OSOE_GET_LANDING_BYTES`) of 256 KiB (the default), 1 MiB,
4 MiB and 16 MiB; then `multistream get` at 8 PEs a node over every node at the
same zones. A get's pieces go out together only as far as the zone holds them -
four pieces at the default - so on a link that a mebibyte crosses in less than
a round trip the zone sets a get's speed; the zone's size for a fast link is the
proposal's (d). 24 jobs: about ten minutes.

## 9. Fan-in

```sh
docs/fast-link/fan-in.sh NODES out/fan-in
```

Every PE of every node but the first puts into the first node's PE 0 at once,
at 1, 4 and 16 PEs a node and at the default receive buffers and four times
their number; the rate, and the receive pool's `-ENOBUFS` completions and
re-arms on the first node (`transport.am.receive_enobufs`, read by `osoe-stat`
while the job sleeps): the shared pool's re-arms grow with the peers sending
into a node at once (scale S3). 36 short jobs: about five minutes.

## 10. The cluster at size

```sh
docs/fast-link/scale.sh NODES out/scale
```

`osoe-bench` at every node with 8 and 16 PEs a node, at no helpers and at §5's
best count; `osoe-pacer` `mix` for ten minutes at both; start-up time at 1, 8
and 32 PEs a node; and the SOS test suite at two PEs a node if its build is
given (`SOS_BUILD`, as `docs/pi-cluster.md` §8 builds it). The first runs of osoe
on x86-64 hardware and on more than four nodes: a failure here is the first
finding to bring back. About an hour.

## 11. What to bring back, and what it replaces

Every `out/` directory, whole, into
`docs/design/evidence/v0.2-step4/<date>-fast-link/` with the kit's version
(`git rev-parse HEAD`) and the facts of §2; then `docs/fast-link-results.md`,
in the form of `docs/pi-cluster-results.md`.

The README's sentences the numbers replace or confirm:

- **The helpers:** the `OSOE_PROGRESS_HELPERS` row, "When to use progress
  helpers", and "No faster link has been measured" (§5);
- **The zero-copy send:** the `OSOE_WIRE_ZERO_COPY` row's "Not timed on a fast
  link: v0.2 step 4's last part, on one, decides its default and whether a
  connection needs more than two buffers" (§7);
- **The ladder:** the `OSOE_AM_BUFFER`, `OSOE_WIRE_BYTES` and
  `OSOE_WIRE_BUFFERS`, and `OSOE_WIRE_UNSENT_BYTES` rows (§6);
- **Gets:** the Limits entry on a large get and the landing zone, and the
  `OSOE_GET_LANDING_BYTES` row (§8);
- **The platforms:** "aarch64 only" where it says so of measured hardware, and
  the Pi cluster as the only real cluster (§3, §10).

The decisions are the author's, each in a dated record beside the results.
