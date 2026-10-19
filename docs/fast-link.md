# Running osoe on a fast link: v0.2 step 4's Part 4

The runbook for the measurements that need a link faster than the Pi
cluster's gigabit: v0.2 step 4's Part 4
(`docs/prompts/done/v0.2-step4-progress-helpers-prompt.md`, "Part 4: gated on a
fast link"), and the questions earlier parts left to it. Written before any such
machine existed, so that one was not paid for while scripts were written: every
section's script has run end to end on the development VM's logical nodes
(`KIT_LAUNCHER=osoe-launch`, `docs/fast-link/dry-run.sh`, at every helper count
the sweep runs; the runs are in `docs/design/evidence/v0.2-step4/`,
`2026-10-10-fast-link-dry-run/` for the kit as its two reads left it,
`2026-10-11-fast-link-kit-fixes/` for the kit as round nine left it, and
`2026-10-11-kit-c4-standard-16/` for the kit as it was set for
`c4-standard-16`; §8's loop over PE counts and §12 to §16, added by the third
cloud session for B14 of `docs/prompts/v0.2-open-items.md`, have run only in
that session's dry run, on an x86-64 sandbox's logical nodes,
`docs/prompts/cloud-session-3-manifest.md`). The first session on a cluster, on 2026-10-12
(`docs/fast-link-results.md`), ran §1 and §2, §3's probe and suite, §4, §6, §8
and §9 on eight `c4-standard-16`; §5, §7 and §10 have run only in the VM, and
so has `verify.sh`, which made that session's by-hand `cluster-verify.sh` a
section of §3. In the development VM's Slurm, one node of four CPUs, the kit's
`launch`, `on` and `spawn` have run, with `facts.sh`'s
`scontrol` and `sinfo`, `helpers.sh`'s jobs and `slurm-setup.sh`'s reading of
`slurmd -C`. Never run at all before that session: the rest of `slurm-setup.sh`,
`build.sh`'s copying to other nodes, a job across nodes under Slurm, and
`floor.sh`'s second direction, which one machine cannot run. The kit was read
adversarially twice before any node was rented
(`docs/reviews/archive/v0.2-fast-link-kit-adversarial-read.md` and
`v0.2-fast-link-kit-second-adversarial-read.md`) and again in round nine's
review (`v0.2-before-part4-review.md`, its third change), and their findings
are in what follows. On 2026-10-11 the kit was set for `c2-standard-8` and,
the author choosing `c4-standard-16` after all, set back the same day, keeping
that change's fixes; the change was checked by a Sonnet agent in place of a
fourth read (`v0.2-fast-link-kit-c2-standard-8-check.md`), and the counts and
figures restored are those the three reads saw.

**Build the commit the run records.** Every section's `run.txt` names the
commit (`git rev-parse HEAD` on the first node, or `KIT_COMMIT` where the tree
has no `.git`); the kit describes the library as it is at the commit it was
merged with, the blocking get's pieces together and the zero-copy send's copy
when locked memory runs out among it.

What Part 4 decides, and from what:

| Question | Where it was left | What decides it here |
|---|---|---|
| Whether a node should run progress helpers by default, and how many | the progress helpers proposal's §8 item 3: no helpers by default, a count from `OSOE_PROGRESS_HELPERS` alone, until a fast link has been measured (`docs/decisions/2026-10-07.md`, items 4 to 6) | §5: the put and get streams and the mailbox at 0, 1, 2 and 4 helpers and 8 to 32 PEs a node, against the bare stream (§4) |
| The zero-copy send's default, and whether a connection needs a third buffer with it on | `OSOE_WIRE_ZERO_COPY`'s row: "Not timed on a fast link" | §7 |
| The ladder's rungs on a fast link: the unsent limit and its rule, the connection buffer, the receive buffers | Part 0's ladder, measured on the Pi and the VM (`docs/decisions/2026-10-07.md`, item 12) | §6 |
| The landing zone's size, which holds a large get | `docs/design/blocking-get-proposal.md`, (d) | §8 |
| The receive pool at many peers sending at once | the review of step 4's merge point, scale S3 | §9 |
| The landing zone's per-node budget (P32's default) | the author's A5, 2026-10-12; H28 | §8 |
| `collect`'s places: where each way wins, and P35's 16 nodes | P31 and P35, provisional; H25 | §12 |
| What waits behind a stream at each helper count | §11's list, once left out; H15 to H19 | §13 |
| The pipeline's window and the chooser's shapes | round four's scale S2; H13 | §14 |
| What a site's latency tuning buys | the cluster's first session; H27 | §15 |
| The ladder's code rungs, 1 and 4 | Part 0's ladder; H6 | §16 |

And beside them, osoe's first x86-64 kernel and its first node counts past the
Pi's four - which the first session had, at eight nodes (§3, §8, §9) - and the
cluster at size (§10).

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
- Every node able to reach every other's address on every TCP port: osoe's
  (`OSOE_PORT_RANGE`, 17000 to 26999 by default), the run's own (§4: 18000 to
  18015 for `echo-uring`, 5201 for `iperf3`), Slurm's (6817 and 6818, and
  `srun`'s own ephemeral ports back to the first node) and `ssh`. A network made
  for the run - for the MTU chosen below - starts with no rule between its
  instances: "allow all TCP between the nodes" is the rule to give.
- One user, with one user id, on every node, since Slurm runs tasks by user id
  (Google's OS Login gives it; accounts made on each instance from metadata
  keys may not), able to `ssh` from the first node, or a head node off the list, to
  every node - the keys that reach the nodes from outside do not reach them
  from there unless the agent is forwarded (`ForwardAgent`) - and with `sudo`
  on every node.
- The tree on the first node, or the head node, by `git clone` or a copy, so that its commit is
  known. `NODES` holds Slurm's names for the nodes: the short host names
  `slurmd -C` reports, not addresses.

**On Google Cloud** - the author's choice of 2026-10-11, after weighing
`c2-standard-8`'s price against these cores (`docs/decisions/2026-10-11.md`,
"The cluster is c4-standard-16 after all"): `c4-standard-16` (16 vCPUs, eight
cores, up to 23 Gbit/s), the `debian-13` image on Hyperdisk, which the C4 series
boots from, the gVNIC interface it requires, and a compact placement policy, at
about $0.79 a node-hour on demand - a list price recalled, not looked up, and to
be checked. Eight nodes for four sessions of four hours is about $100. The
account is the author's, and so is every launch. The sweep's 8, 16 and 32 PEs a
node are one PE a core, one a vCPU and two a vCPU on it. Make the VPC with an
MTU of 8896, the largest Google allows (its default is 1460): the author's
decision of 2026-10-11 (`docs/decisions/2026-10-11.md`, item 13), the same in
every session, since a change between sessions makes their numbers
incomparable. §2's facts record the MTU each node has.

**Sessions.** Each section says how long it takes; in four-hour sessions:

1. Setup, facts, the suite on a node, the floor and the bare stream (§1 to §4).
2. The helper sweep (§5), about an hour and a quarter at eight nodes, with
   time to repeat what it shows.
3. The ladder, the zero-copy send, the landing zone, the fan-in (§6 to §9).
4. The cluster at size (§10), and whatever the first three left to repeat.

Bring everything back at the end of each session (§11): the machines are
deleted between sessions, and nothing on them survives - so each session
starts with §1 and §2 again, about half an hour, which the plan above does not
count.

## 1. Setup

The kit is this file's directory, `docs/fast-link/`. On the first node, which
also runs Slurm's controller - or on a head node off the list, which runs the
controller, the builds and the lead's `ssh` so that no measured node does - as a
user with `sudo`:

```sh
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake pkg-config \
    liburing-dev git rsync ethtool iperf3
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends slurm-wlm munge
```

and the same on every other node (the list `NODES`, one name a line, every
node's name as the others resolve it, the first node first).
`DEBIAN_FRONTEND=noninteractive` answers `iperf3`'s question about a daemon of
its own with no, and Slurm goes without its recommended packages: they bring
the ROCm GPU plugin and its library, `librocm-smi64`, and with the library
installed `slurmd -C` - from which `slurm-setup.sh` writes each node's line -
crashed in the development VM, a machine without a GPU, printing no node line;
with the plugin removed alone it still did (round nine's review, R2):

```sh
for n in $(grep -vxF "$(hostname -s)" NODES); do ssh -o StrictHostKeyChecking=accept-new $n \
    "sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
     build-essential cmake pkg-config liburing-dev rsync ethtool iperf3 &&
     sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
     slurm-wlm munge"; done
```

Then, on the same machine:

```sh
docs/fast-link/slurm-setup.sh NODES       # munge's key, slurm.conf, the daemons, on every node
CONTROLLER=$(hostname -s) docs/fast-link/slurm-setup.sh NODES   # instead, on a head node off the list
docs/fast-link/build.sh NODES             # the tree to every node at the same path, built there
```

`slurm-setup.sh` writes one partition of every node with its cores and memory
as `slurmd -C` reports them - by its path, `/usr/sbin/slurmd`, which Debian 13
keeps off a user's `PATH`, and it stops unless every node gave its line, saying
what a node that gave none printed and what to do -
`PropagateResourceLimitsExcept=MEMLOCK`, so that a step takes the login
session's limits but the locked-memory one, which `slurmd` gives it unlimited,
and `TaskPlugin=task/none`, so that nothing binds a step's tasks: under
`task/affinity` a job of one task - as `floor.sh`'s servers, clients and
`iperf3` each are - is confined to the core Slurm allocated it, whatever
`--cpu-bind=none` says, and §4's yardstick would measure a core a side rather
than the link (round nine's review, R1: the confinement measured in the
development VM's Slurm, what it costs the yardstick not). Every job is then
unbound, so the fast link's numbers are unbound ones - where the README gives
the helpers' costs bound and unbound, and where the decisions index's way past
the Pi's barrier's two modes, Slurm's default binding, is not to be had: read
the barrier and the 8-byte reduction pooled, or by mode (§5). It also sets, on
every node:

- `RemoveIPC=no` for systemd-logind: with the default, a user's files in
  `/dev/shm` go ten seconds after the user's last session on a node ends, and a
  job still in `shmem_init` then waits for ever (`docs/pi-cluster.md`, §1);
- an unlimited locked-memory limit (`memlock`) for the user and for `slurmd`'s
  steps: io_uring charges a node's rings to it on Linux 6.14 and later, and the
  zero-copy send each send it lends until the notification, against a default
  of 8 MiB that every process of the user on a host counts toward (§7).

Read what it prints. `build.sh` copies the tree with `rsync` to the same path on
every node, builds the release preset on each in parallel, and compiles the
kit's probes beside it, into `build/release/kit/`: everything a step runs is at
the same path on every node, without a shared file system. A node whose build
fails says so, removes its `build/release/kit/` so that nothing runs the last
build unnoticed, and makes the script's status the failure.

**Every job** the kit starts runs with `OSOE_NO_PROGRESS_TIMEOUT=60`, as the
README asks while a cluster is new, so that a stalled wait ends the job and
names what it waited for; with `SHMEM_DEBUG=1`, since each job measures the
link at start-up and sets every data wire's unsent limit from what it measured,
and its start-up lines - PE 0's link, each node's limit - record both for every
job, where only §7's and §9's `osoe-stat` dumps did before, and `summarize.py`
prints them beside each case (round nine's review,
S2); under a timeout of about three times its length, each section's own
(`KIT_TIMEOUT`), past which it is killed and `osoe-stat --gc` removes what it
left in `/dev/shm` on every node; and with its input from `/dev/null`. Each job
is an allocation of its own, of whole nodes, whose one step starts the PEs
unbound and with more PEs than CPUs allowed (`common.sh`'s `launch`):

```sh
salloc -J osoe-kit -N<nodes> --exclusive \
    srun -K1 --cpu-bind=none --overcommit -N<nodes> --ntasks-per-node=<PEs> PROGRAM
```

32 PEs a node are more tasks than a `c4-standard-16` has CPUs, and Slurm
refuses a job that many, whatever `--overcommit` says - `select/cons_tres`
tests a node's tasks against its CPUs before it looks at the overcommit - where
a step of an allocation may have them (round nine's review, P9-2; measured in
the development VM's Slurm 24.11.5, eight tasks on four CPUs). Every job is
named `osoe-kit`, so that one the timeout ended, whose killed `salloc` may
leave its allocation behind, is cancelled by name - and `slurm-setup.sh` sets
`InactiveLimit=120` besides. Run the kit from the first node's shell, not
inside an allocation: each job asks for whole nodes of its own, which an
allocation around the kit would hold. Each section's output directory should
be new: a second run into one overwrites its rounds by name.

## 2. The facts

```sh
docs/fast-link/facts.sh NODES out/facts
```

On every node: the kernel, the CPU (`lscpu`), memory and `/dev/shm`, the page
size, the governor, the interface's driver, speed, MTU, offloads, interrupt
coalescing and queues (`/usr/sbin/ethtool -i/-k/-c/-l`), the TCP and core
network settings, read from `/proc/sys` (`net.core.netdev_max_backlog`,
`rmem_max`, `wmem_max`, `net.ipv4.tcp_rmem`, `tcp_wmem`,
`tcp_congestion_control`, `tcp_notsent_lowat`, `tcp_slow_start_after_idle`,
`net.core.somaxconn`, and busy polling's `net.core.busy_read` and `busy_poll`,
which §15 sets), the locked-memory, open-file
and stack limits a Slurm step sees, `io_uring_disabled`, `perf_event_paranoid`
(which §13's profile meets), logind's `RemoveIPC`,
liburing's version, each node's name as every other resolves it, and Slurm's
configuration. A fact that cannot be read says why in its line. Kept with the results: a number without them is not comparable
with another.

## 3. The suite on a node, the probe on every node

```sh
srun -N$(wc -l < NODES) --ntasks-per-node=1 build/release/tools/probe/probe
salloc -N$(wc -l < NODES) --exclusive srun --cpu-bind=none --overcommit -N$(wc -l < NODES) \
    --ntasks-per-node=32 build/release/kit/startup
srun -N1 -n1 -c16 ctest --test-dir build/release -L tier2 -E '^model\.' -j8 --timeout 300 > out/suite.txt
```

The probe must say `ok` for every facility on every node, and the second line
must start a job of 32 PEs a node, the sweep's largest, as the kit starts every
job (§1), before the sweep is started. The suite is the
release build's tier 2 on one node's logical nodes; tier 1 compiles, and runs on
the node that built it (`ctest -L tier1`). The floor pins (`shmem.floor`,
`echo-uring`'s) were set on the VM and may fail here: a finding to keep, not a
reason to stop. About twenty minutes.

The whole verification - round nine's in the development VM, split by part -
runs as Slurm batch jobs, one a part on a node of the list, each node whole, so
that `squeue` shows them and the kit's own jobs wait for them:

```sh
docs/fast-link/verify.sh NODES ~/verify-out     # debug release asan tsan tier3-single models pacer-0 pacer-2
for n in $(cat NODES); do mkdir -p out/verify/$n; ssh $n tar -C verify-out -cf - . | tar -x -C out/verify/$n; done
```

Each part copies the tree to `~/verify-tree-PART` on its node and builds there,
leaving the kit's build alone, and writes `~/verify-out/PART/` there:
`verify.txt` its verdicts, the full outputs beside it; the second line brings
them back once the jobs are done. Parts may be named instead - `static`, the
release build's static checks alone, is the one to run first on a new tree -
and more parts than nodes queue on them in turn. The models run several at once
where a node has the cores and the memory (`VERIFY_MODEL_JOBS`, in the script's
header): on the first session's nodes, one at a time, `model.reservation` alone
took 1,113 s, and the half of the models that held it outlasted the session.

## 4. The floor and the bare stream

```sh
docs/fast-link/floor.sh NODES out/floor
```

Between the first two nodes: `shmem_test_floor`, one PE a node - the library's
get, put and quiet, fetch-add and stream against an io_uring echo and stream
between the same two processes (design §8.4); then 1, 2, 4, 8 and 16 streams
each way at once, both nodes sending as `multistream` makes every node send:
`iperf3 --bidir`, whose sum over its ten seconds is the bare aggregate - what a
node with several connections can reach, which one connection's rate may not
show where a provider limits each flow - and `echo-uring`, a pair of processes
a connection, whose per-connection medians, summed, are an upper bound beside
it, since nothing checks that its streams ran at once. Every process is
unbound, free to use every core of its node - each a job of one task, which
`slurm-setup.sh`'s `TaskPlugin=task/none` leaves unbound (§1); a client whose
server is not yet listening tries again, and servers whose client never came
are ended. About fifteen minutes.

## 5. The progress helpers

```sh
docs/fast-link/helpers.sh NODES out/helpers          # three rounds, 0 1 2 4 helpers, 8 16 32 PEs a node
docs/fast-link/summarize.py out/floor out/helpers
```

Each case is a fresh job at one helper count (the count shapes the node's
connection area and is agreed at start-up), every node at once:

- `multistream put 1048576 10`: every PE puts 1 MiB blocks to the PEs of every
  other node for ten seconds, a quiet every 8 MiB - consecutive blocks to
  different nodes, and a node's PEs starting on different ones, so that every
  peer of a node carries traffic at once, which is what helpers divide; the
  job's and each node's rate, each node's bytes to each peer, and the CPU each
  node's progress threads - its PEs' agents, by the name `osoe-agent`, the
  lead's doing the work and the others waking ten times a second, and its
  helpers, `osoe-helper-N` - and its PEs' own threads spent, a GiB moved. The
  kernel's receive work in softirq context is in neither. And how many peer
  nodes a node's PEs are inside a call toward at once, `peers_in_calls`: each
  PE records when each of its calls began and returned, and toward which node,
  and after the window the node's first PE sweeps its node's records for the
  time-averaged count of distinct peer nodes some PE of the node is in a call
  toward, over the window in which all of them are in their loops; and the same
  with each quiet counted toward the block before it,
  `peers_in_calls_with_quiets`, and the quiets' share of the PEs' time. Neither
  is the model's quantity exactly (the kit's second adversarial read, its
  Appendix), which counts every PE on the wire of its current block, with no
  quiets and no buffers: `peers_in_calls` is a lower bound of it, pulled down by
  the PEs' time in quiets and by a put's bytes still draining after its call
  returns, and the count with quiets comes nearer. So the model's numbers - the
  second read's, which the model rebuilt in
  `docs/design/evidence/v0.2-step4/2026-10-11-kit-c2-standard-8/` reproduces -
  go beside the pair: at eight nodes about four of seven at 8 PEs a node, five
  at 16, six at 32; at five, 2.7 to 3.0 of four at 8 PEs a node and 3.6 to 3.9
  at 32 - where the first order of the kit's had, at eight nodes, two to three
  at 8 PEs a node and under two at 16 and 32. In the development VM at eight nodes of 8 PEs they
  sat near or within the pair at 1 MiB, and near the count with quiets at
  64 KiB, whose quiets took a quarter to a third of the PEs' time and left
  `peers_in_calls` at 2.4 to 3.0 (round nine's read of these changes, finding
  1): a convoy lowers both figures, long quiets only the first. At eight
  nodes four helpers are then about four fifths busy at 8 PEs a node, nine
  tenths at 16 and nearly all at 32.
  §8's rows of the same figures fall as the zone grows, a get's wait moving
  from its calls to its quiets, and are not read there. (The figure the kit
  printed before, a bit a block in the millisecond its call returned, counted
  the blocks a node finishes in a millisecond, its rate, and is gone: round
  nine's review, S1.)
- `multistream put 65536 10`, the same in 64 KiB blocks: more messages a byte;
- `multistream get 1048576 10`: the same with nonblocking gets, a quiet every
  8 MiB;
- `osoe-pacer --seconds 20 mailbox`: small messages with signals to PEs on every
  node, messages a second;
- `osoe-bench quick barrier collectives`: the barrier, a sum reduction and a
  broadcast of 8 bytes and of 64 KiB, and an fcollect and an alltoall of 8-byte
  and of 4 KiB blocks, over every PE, each line the median of 200 calls after
  200 of warm-up. A collective's payloads go on the data wires, which the
  helpers hold, and on the Pi cluster two helpers made the mailbox about a fifth
  faster while a 64 KiB broadcast and a 4 KiB alltoall ran 6 to 35% slower
  bound and 2 to 9% unbound (round eight's review,
  `docs/reviews/archive/v0.2-step4-scale-and-performance.md`, S4; the README's
  Limits): a sweep of the streams and the mailbox alone would have read them as
  a gain there (round nine's review, S3). The 64 KiB broadcast's tree is
  chosen from each job's own measured link, which `summarize.py` prints beside
  the row: compare rows whose links agree, or set the link for the workload
  (`OSOE_LINK_*`, from §2's facts) where they do not (round nine's read of
  these changes, note 3).

The order of the helper counts changes each round, so that a drift in the
machines does not read as a helper count's. Three rounds of 4 helper counts × 3
PE counts × 5 workloads are 180 jobs and their start-ups: 144 of 10 to 20
seconds, and 36 of the collectives, which take seconds - their longest line, the
4 KiB alltoall at 32 PEs a node, sends 28 MiB from each node a call, some 10 ms
at 23 Gbit/s, 4 s over its 400 calls. About an hour and a quarter at eight
nodes. `ROUNDS`, `HELPERS`, `PES_PER_NODE` and `WORKLOADS` in the environment
shorten it.

`summarize.py` prints, for each workload, PE count and helper count, the median
and range of the rounds' figures, to three significant figures, with the count
of rounds - a case with fewer lost one: the rates - the job's, and a node's,
which is what the node sent to all its peers, one way - the progress threads'
CPU a GiB, the peers in calls and the quiets, the mailbox's messages a second
and each collective line's time; beside each case, the link its jobs measured
at start-up and the unsent limit they set from it (§1); and, from `out/floor`,
§4's bare aggregate between two nodes both ways, with a node's share each way,
which is what a node's rate is set beside, at as many streams as a node has
peers. What decides the question (§8 item 3) is whether a helper count raises
the rate a node reaches toward that share, at what CPU, and whether that holds
at every PE count - weighed against what it costs the collectives: the 64 KiB
broadcast's and the 4 KiB alltoall's lines, where the Pi's helpers cost, beside
the streams; the barrier's and the 8-byte reduce's read pooled or by mode,
since unbound they land in one of two modes a job on the Pi, about 25 µs apart,
which three rounds can draw unevenly (`docs/decisions/README.md`). A rule the
author may then give: for example a count that follows the link's measured
bandwidth and the node's peers, or the setting alone as today.

**H26, the helpers under a site's binding** (B11). Most sites run Slurm with
`task/affinity` and `task/cgroup`, each task bound to the cores it was allotted,
where the kit runs `task/none`. `docs/fast-link/binding.sh NODES site` switches
every node to that binding - it restarts slurmctld and every slurmd, and ends with
a job's two tasks' allowed CPUs, one core each - and `binding.sh NODES none`
switches back, which every other section needs. Under it, run this section's
streams and collectives at 0, 1, 2 and 4 helpers at one count of PEs, Slurm's
binding left on (`KIT_SRUN='-K1 --overcommit'`, without `--cpu-bind=none`), and
again with a core reserved for the helpers (`--cpus-per-task=2`), beside the
unbound rows above: a helper shares its PE's core unless the job reserves one.
Tried on the development VM's one-node Slurm, both ways (2026-10-13).

## 6. The ladder

```sh
docs/fast-link/ladder.sh NODES out/ladder
```

Part 0's rungs that a setting switches, eleven cases among which the default
runs once a round, in another order each round, three rounds, between the first
two nodes; every case under `SHMEM_DEBUG`, whose start-up line records the
unsent limit its own link measurement gave:

- **The unsent limit** (`OSOE_WIRE_UNSENT_BYTES`): the default - the rule's
  limit from the measured link, which `SHMEM_DEBUG` reports and the script
  records - against 0 (the system's), 64 KiB, 256 KiB, 1 MiB and 4 MiB. Each
  with `hol` (a fetch-add's round trip behind a 1 MiB put stream on the same
  connection, four PEs, two a node) and the stream's rate: the limit trades the
  one for the other, and its rule's 128 µs of turnaround was a guess no one had
  timed on a fast link (the scale review, S5); the first session did, and a
  lower limit beat the rule a little (`docs/fast-link-results.md`).
- **The connection buffer** (`OSOE_AM_BUFFER`): 128 KiB, 256 KiB (the default),
  1 MiB and 4 MiB, with the stream and `hol`.
- **The receive buffers** (`OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS`): 16 KiB × 64
  (the default), 64 KiB × 64, 16 KiB × 256, with the stream.

Rungs 1 (fill the room) and 4 (parse in place) are code, not settings: measuring
them without means building the trees before them, which §16 does (`3ee3357`
and `de3dc42` for rung 1, `49c62d3` and `2dcf8d6` for rung 4). 33 jobs of about
fifteen seconds: about ten minutes.

## 7. The zero-copy send

```sh
docs/fast-link/zero-copy.sh NODES out/zero-copy
```

`multistream put` at 1 MiB and 64 KiB, with `OSOE_WIRE_ZERO_COPY` off and on, and
on at a threshold (`OSOE_WIRE_ZERO_COPY_BYTES`) of 16 KiB, 64 KiB and 256 KiB;
with the rate, the progress threads' CPU a GiB - which is what the zero-copy
send exists to save - and the node's counters of its sends, those by reference
among them, their notifications and those the kernel copied after all
(`osoe-stat`; a refused send counted among those by reference, its
notification among those copied). It records the locked-memory limit each step
saw: below what a send needs, it is refused, goes again by copy, and its
connection copies from then on, which the notice "refused for want of locked
memory" says; a run that shows it measured the copy. `HELPERS` is no helpers
unless given: `HELPERS="0 N"`, N the count §5 found best, makes the 48 jobs
this section is planned as, 24 at one count: ten to fifteen minutes.

## 8. Gets, and the landing zone

```sh
docs/fast-link/gets.sh NODES out/gets
```

Question 4's probe (`get-pieces`), one PE on each of two nodes: a put and
quiet, a blocking get, and a nonblocking get and quiet, from 16 KiB to 64 MiB,
at each landing zone of `ZONES`; then `multistream get` over every node at each
count of `PES_PER_NODE`, at the same zones. A get's pieces, a blocking get's as
a nonblocking get's, go out together only as far as the zone holds them - four
pieces at 256 KiB, sixteen at 1 MiB - so where a round trip's time carries more
than the zone, the zone sets a get's speed. The first session ran zones of 256
KiB (the default then), 1, 4 and 16 MiB at 8 PEs a node (H9); on its numbers
the author made the default a node's budget, 4 MiB split among its PEs, between
256 KiB and 1 MiB a PE (P32: 1 MiB at one to four PEs a node, 512 KiB at eight,
256 KiB at sixteen). This section now measures that budget (H28): the zone
unset, "default" - the script unsets `OSOE_GET_LANDING_BYTES`, so the library's
own is what runs, and each PE's start-up line names the zone it had - beside
256 KiB and 1 MiB fixed, at 1, 4, 8 and 16 PEs a node; the first session's
sweep is `ZONES="262144 1048576 4194304 16777216" PES_PER_NODE=8`. The pieces in
flight toward a node are bounded by its connection's reply credits too
(`OSOE_AM_CREDITS`, 64 a connection, shared by the node's PEs), which this
does not vary: with the stream's blocks spread over the peers, a connection
carries about 8/7 of one PE's pieces at once at eight nodes of 8 PEs, so the
credits
hold a zone back only from a few MiB. `get-pieces`' one PE, alone on its
connection, has at most 64 pieces of 64 KiB, 4 MiB, in flight: past a zone of
4 MiB its line is the credits', not the zone's. 45 jobs: about fifteen
minutes.

## 9. Fan-in

```sh
docs/fast-link/fan-in.sh NODES out/fan-in
```

Every PE of every node but the first puts into the first node's PE 0 at once,
at 1, 4 and 16 PEs a node - 320, 80 and 20 iterations, so that each job moves
about as much - and at the default receive buffers and four times their
number; the rate, and the receive pool's `-ENOBUFS` completions and
re-arms on the first node (`transport.am.receive_enobufs`, read by `osoe-stat`
while the job sleeps): the shared pool's re-arms grow with the peers sending
into a node at once (scale S3). 36 short jobs: about five minutes.

## 10. The cluster at size

```sh
docs/fast-link/scale.sh NODES out/scale
```

`osoe-bench` at every node with 8 and 16 PEs a node, at the counts of `HELPERS`
- no helpers unless given, so `HELPERS="0 N"` with §5's best; `osoe-pacer`
`mix` for ten minutes at each; start-up time at 1, 8 and 32 PEs a node - the
slowest PE's, which is the launcher's spread and init, and the fastest's,
nearer init's own; and the SOS test suite at two PEs a node if `SOS_BUILD`
names the directory of its programs, built as `docs/pi-cluster.md` §8 builds
them - the suite fetched into the reading corpus first, which the repository
does not hold - at the same path on every node: inside the tree but not under
`build/`, and built before `build.sh` runs, or with `build.sh` run again after
it, so that `build.sh` copies it. The first session ran osoe on x86-64 hardware
and on eight nodes already (§3, §8, §9); here `osoe-bench`, `osoe-pacer` and SOS
run across them for the first time, and a failure is the first finding to bring
back. About an hour.

## 11. What to bring back, and what it replaces

Every `out/` directory, whole, into
`docs/design/evidence/v0.2-step4/<date>-fast-link/` with the kit's version
(`git rev-parse HEAD`) and the facts of §2; then `docs/fast-link-results.md`,
in the form of `docs/pi-cluster-results.md`.

The README's sentences the numbers replace or confirm:

- **The helpers:** the `OSOE_PROGRESS_HELPERS` row, the "What does not scale"
  entry on helpers, "What they buy with many peers at once ... has been
  measured once, in a fan-in" and the sweep that "has not run", the wake's cost
  at many PEs a node, and the sentence on Google Cloud's link after "says
  nothing above it" (§5);
- **The zero-copy send:** the `OSOE_WIRE_ZERO_COPY` row's "Not timed on a fast
  link: v0.2 step 4's last part, on one, decides its default and whether a
  connection needs more than two buffers" (§7);
- **The ladder:** the `OSOE_AM_BUFFER`, `OSOE_WIRE_BYTES` and
  `OSOE_WIRE_BUFFERS`, and `OSOE_WIRE_UNSENT_BYTES` rows (§6);
- **Gets:** the Limits entry on a large get and the landing zone, and the
  `OSOE_GET_LANDING_BYTES` row (§8);
- **The platforms:** the opening's "largest job across separate machines", and
  the Limits entry "Tested on aarch64, and on x86-64 since 2026-10-12" (§3,
  §10);
- **collect's places:** the `OSOE_COLLECTIVE_PLACES` row's defaults, "the
  exchange on no more than 16 nodes" and "provisional ... until H25", and
  design §5.5's (§12);
- **The landing zone's budget:** the `OSOE_GET_LANDING_BYTES` row's default and
  the Limits entry on a large get's "which H28 is to measure" (§8);
- **The shapes:** the `OSOE_COLLECTIVE_SHAPE`, `OSOE_COLLECTIVE_FANOUT` and
  `OSOE_COLLECTIVE_PIPELINE` rows, where the chooser's costs were fitted to the
  Pi (§14);
- **Behind a stream and tuned:** the "What does not scale" entry on helpers and
  the README's latency sentences, with what §13 and §15 find.

What the kit once left out is in it now: a barrier, a fetch-add and the
collectives behind a stream at each helper count, TCP's retransmissions around
a stream and a profile of the progress threads (§13), the pipeline's window and
the chooser (§14), latency tuning (§15) and the ladder's code rungs (§16). Not
in it: io_uring's task-run flags and registered files (§17).

The decisions are the author's, each in a dated record beside the results.

## 12. collect's places

```sh
docs/fast-link/collect.sh NODES out/collect
```

H25: the crossover among `collect`'s four ways to its places, which H24
measured only on one machine's logical nodes, whose CPUs carry every lead's
messages at once (`docs/design/evidence/v0.2-step4/2026-10-13-h24/`).
`osoe-bench places` - a `collect` of differing blocks over the world, three
patterns of lengths: every PE's 8 bytes, every sixteenth PE's alone, and PE 0's
64 KiB beside every other's 8 - by each way forced (`OSOE_COLLECTIVE_PLACES`:
`members`, `totals`, `scan`, `exchange`) and by the default, `link`, the way
the link makes cheapest, the leads' exchange on no more than 16 nodes (P35,
provisional until this), every node at once at 8, 16 and 32 PEs a node, three
rounds, the ways in another order each round. `tools/bench/collect-places.sh`
is the same measurement at one PE a node on logical nodes; this is it as the
kit runs its jobs. `summarize.py` sets the ways' medians side by side for each
PE count and pattern, and beside them each default job's link and the way
`shape_timing.py`'s `choose_places` takes on it, the C's own (held to it by
`shmem.shape.places-grid`): where a way the chooser does not take beats the
one it takes, its costs, or P35's 16, are what to move. 45 jobs of a few
seconds: about five minutes.

## 13. Behind a stream, at each helper count

```sh
docs/fast-link/behind.sh NODES out/behind
```

What §5 times alone, behind a stream (H15 to H19), every node at once, at 0, 1,
2 and 4 helpers, three rounds, the counts in another order each round:

- **hol's operations** (`hol SECONDS OPERATION [BYTES]`), two PEs a node: each
  node's first PE streams 1 MiB puts to the next node's, all but the last
  node's, while PE 1 times an operation alone and then behind the streams - a
  fetch-add to PE 3, behind PE 0's stream on the same connection, at each count
  (H16, where §6 has it at none); the barrier of the nodes' second PEs, whose
  messages go on the control wires the lead's agent keeps, so that a helper
  count reaches it only through the CPU the helpers take (H15); and their sum
  reduction and broadcast of 8 bytes and of 64 KiB, whose payloads go on the
  data wires the helpers hold (H17). The team runs as many operations as PE 1
  sets from the operation's time alone, and only those that end while PE 0's
  stream runs count toward the stream's line.
- **TCP's counters around a stream** (H18): `nstat -n` on every node, then
  `multistream put` in 1 MiB blocks at 8 PEs a node, then `nstat` on every
  node - its retransmissions (`TcpRetransSegs`, `TcpExtTCPLostRetransmit`,
  `TcpExtTCPSackRecovery` and the rest) since, which would explain a cloud
  link's rate.
- **The progress threads profiled** (H19): the same stream with every node's
  CPUs profiled for five seconds, three seconds in (`profile.sh`: `perf record
  -a -g`, then its report by thread name, library and symbol, so that
  `osoe-agent` and `osoe-helper-N` are told from the PEs' own). It is a step of
  the job's own allocation beside the job's (`common.sh`'s `beside`, `srun
  --overlap`), since a job holds its nodes whole. `perf` must be installed
  (`linux-perf` on Debian) and allowed every process: `PERF="sudo -n perf"`, or
  `kernel.perf_event_paranoid` at -1, which facts.sh records; where it is not,
  the profile's file says why and the section goes on.

`summarize.py` gives hol's lines by operation and helper count, the streams'
rates, and the counters as `nstat` printed them. Four helpers need four peers,
five nodes. 40 jobs of about twenty seconds and 16 of a few: about twenty
minutes.

## 14. The pipeline's window and the chooser

```sh
docs/fast-link/window.sh NODES out/window
```

H13: the large reduction's and broadcast's time (`shapes`: 64 KiB, 256 KiB,
1 MiB and 4 MiB, four to 256 of the leads' 16 KiB pieces), every node at once
at 8 PEs a node, by the chooser - `OSOE_COLLECTIVE_SHAPE=link`, the default, a
tree's fan-out and window or Träff's from the three numbers of the link each
job measures at start-up - and by `tree`, its trees alone, beside every fixed
shape of fan-outs 2, 4 and 8 and windows 1, 2, 4 and 8
(`OSOE_COLLECTIVE_SHAPE=fixed`, `OSOE_COLLECTIVE_FANOUT`,
`OSOE_COLLECTIVE_PIPELINE`), three rounds in another order each. The chooser's
costs were fitted to the Pi's table (`shape_timing.py`); where a fixed shape
beats the one it takes at a size, they are off on this link, and the
`OSOE_COLLECTIVE_*` rows say so. 42 jobs of about fifteen seconds: about ten
minutes.

## 15. Latency tuning

```sh
docs/fast-link/tuning.sh NODES out/tuning
```

H27: what a site's latency tuning gives - interrupt coalescing off (`ethtool -C`:
`adaptive-rx` and `adaptive-tx` off, `rx-usecs` and `tx-usecs` 0) and busy
polling on (`net.core.busy_read` and `busy_poll`, 50 µs) on every node, by
`sudo -n` (`tuning-node.sh`), each node's settings before kept and put back at
the end, and when the script is ended - beside the image's settings, the two in
another order each round, three rounds: `ping`'s round trip from the first node
to the second, 200 at 10 ms (0.27 ms on average at the image's settings and
0.14 at best with coalescing off, on 2026-10-12); `osoe-bench`'s latency,
atomic and barrier, one PE on each of the two (osoe's hop, 148 µs on idle nodes
against 47 on busy ones that day); `hol`'s fetch-add alone and behind a stream;
the mailbox on every node at 8 PEs; and §4's floor whole, into
`floor-STATE-rROUND/` (`FLOOR=0` leaves it out). Each node's line in
`tuning-*.txt` says the settings it then had. About forty minutes with the
floor, ten without.

## 16. The ladder's code rungs

```sh
docs/fast-link/trees.sh NODES out/trees
```

H6: §6's two rungs that are code, not settings - rung 1, a put fills the room
the open buffer has (`de3dc42`, beside `3ee3357` before it), and rung 4, the
pass parses each provided buffer where the kernel put it (`2dcf8d6`, beside
`49c62d3` before it) - Part 0's trees beside the kit's own. The script writes
each tree into `build/trees/COMMIT` by `git archive`, so it runs where the
checkout has its `.git` (the first node, or a head node off the list), copies
`build/trees` to every other node as `build.sh` copies the tree, and builds
each tree there with the kit's probes compiled by that tree's own `oshcc`;
`BUILD=0` takes the trees already built. Then, between the first two nodes,
two PEs each, every tree and the kit's own in another order each round, three
rounds: `multistream put` at 64 KiB and at 1 MiB blocks - the rungs' own
measures (`docs/decisions/2026-10-07-one-thread-profile.md`) - `hol`'s
fetch-add alone and behind the stream, and the mailbox. Each case's files
carry its tree's commit, `main` for the kit's. Building four trees takes a few
minutes a node; the 60 jobs about fifteen.

## 17. Not in the kit: io_uring's flags

H7 - io_uring's `COOP_TASKRUN` and `DEFER_TASKRUN` and registered files on a
fast link - needs the library changed: Part 0 measured them in experiment
builds whose choice was an environment variable, and merged none
(`docs/decisions/2026-10-07-one-thread-profile.md`, "Measured and not taken"),
so `main` has no setting that turns them on, and no commit holds the
experiments. A section for them waits for that setting, which is the author's
to ask for.

## 18. Leads claiming team blocks at once (P9)

```sh
docs/fast-link/claims.sh NODES out/claims
```

P9's change (`docs/decisions/2026-10-19.md`): a team block's claim takes the
highest free block of its node's pool, and a swap lost to another lead's claim
or return in the same bitmap word moves it to a word chosen by the claiming
PE's local id, where before it tried the top word again - so that leads
claiming at once spread over the pool. Its contention needs many PEs claiming
at once on one node, which four vCPUs do not make; this times it before and
after, on the list's first node alone. `claims` (`claims.c`): every PE splits
`SHMEM_TEAM_SELF` - a team of its own, which it leads on its node and so
claims a block for - `KEEP` times at a start PE 0 sets 2 ms ahead, so that the
claims begin together rather than as a barrier lets the PEs go, 1,000 times;
it prints the last PE's splits done after the start and a split's time a
claim, each the job's median and percentiles. The tree before the change
(`BEFORE`, `821a6dfb`) is written by `git archive`, so this runs where the
checkout has its `.git`, as §16 does, and built on the first node with
`claims.c` compiled by its own `oshcc`; then 16, 32 and 64 PEs, one, two and
four a vCPU, at one and at eight blocks a PE, the two builds in another order
each round, three rounds: 36 jobs of a few seconds, and the build, about ten
minutes.
