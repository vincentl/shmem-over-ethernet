# The large-cluster playbook: osoe at scale, correctness first

*Written by the lead on the night of 2026-11-01, the day after 0.2.0 was
tagged, at the author's ask: a playbook for an agent that clones this tree onto
a large HPC cluster - hundreds to thousands of nodes under Slurm, a shared file
system, probably no root, probably x86-64, perhaps NICs that osoe does not use.
It says what to run and in what order, where the code is likely to bend, and
where a different algorithm might be worth considering. It builds on
`docs/prompts/off-network-cluster-brief.md`, the measurement session written
for such a cluster as the 0.2 tag's last item, and does not repeat it: that
brief is this playbook's phase B, and its branch, its return file and its
rules hold here unless §6 says which the author relaxed. Four Sonnet agents
surveyed the code for it - the transport and start-up, the collectives, the
data path and memory, and the tests and the kit - and the lead checked what it
cites. The same night a fresh agent read the draft adversarially
(`docs/reviews/archive/large-cluster-playbook-adversarial-read.md`: 25
findings, 22 accepted whole or in part) and this is the text after them; the
questions it left for the author, and his answers of the next morning, are in
`docs/decisions/2026-11-01.md`, and this text carries the answers: the
campaign is two sessions, the grants are G2 to G4, and the `srun` test
harness, a small-collective cost model and the models of four design changes
are built here before any session is given this file.*

## 0. What this is

osoe is correct, as far as it has been tested, on four Raspberry Pi nodes, on
eight Google Cloud nodes, and on 254 logical nodes of one machine; its
ceilings are far above that, and most are held by arithmetic alone
(`docs/ceilings.md`). A large cluster is the first place where the arithmetic
meets a kernel, a switch and a scheduler. The playbook has three phases, in
order, and a session does not start the next until the one before has either
passed or left a recorded failure:

- **Phase A, the gates**: does the tree run correctly here, at all, and then
  across machines? Each gate has a pass condition and a stop rule. The gates
  time a few things - the floor, the eight-node yardstick - but as yardsticks
  for the ladder, not as results: no number from phase A goes in a table
  before A6 and A7 have passed.
- **Phase B, the ladder**: the measurements, at node counts doubling to the
  whole cluster and at PE counts a node rising to the site's cores. The
  off-network brief's seven questions are its core; this playbook adds the
  rungs around them.
- **Phase C, the campaign**: where the runs are likely to show the code bending,
  what change each would call for, and which collectives could take a different
  algorithm. Each is a proposal with its evidence, modelled first where it is a
  protocol, and the author's to decide.

The rules do not change with the cluster's size. Correctness first: a run that
gives a wrong answer, hangs or dies stops the ladder where it is, and the
failure is recorded before anything else runs. A protocol the project changes it
models first (`tools/modelcheck/README.md`). A default, a protocol or a decision
in force (`docs/decisions/README.md`) is the author's: the session describes the
change and its evidence as a question and does not make it. Names are words
(`CLAUDE.md`); a change to the library comes with a test that fails without it;
the README stays true of the tree at every commit.

## 1. Read first

The off-network brief's "Read first" list, in its order, and then:

1. `docs/fast-link-results.md`, the third session (2026-10-28) whole: H11 and
   H12 are the yardstick every number of phase B is set beside - eight nodes at
   8 and 16 PEs a node, and two nodes at 16 to 256.
2. `docs/release-notes/0.2.0.md`, "Known limits": what 0.2 says of itself.
3. The README's `OSOE_AGENT_CPUS` and `OSOE_PROGRESS_HELPERS` rows (Reference,
   "Environment variables"): under a site's binding of a task to one core, the
   lead's main thread, its agent and every helper share that core. And its
   "Starting PEs without a launcher": the `OSOE_*` variables are read only as a
   whole set, in place of Slurm's, never beside them.
4. `docs/fast-link/common.sh` whole: `launch` is unbound by default
   (`KIT_SRUN='-K1 --cpu-bind=none --overcommit'`), makes an allocation a job,
   kills a job at `KIT_TIMEOUT` (900 s) and runs `osoe-stat --gc` only after a
   kill of its own, by an `srun` outside the allocation.
5. The design notes' §3.12 (progress threads and owners), §5.3 and §5.8 (the
   collectives' algorithms, as built), §6.2 (the rendezvous and the mesh) and
   §10 (the open questions), each for the question a run raises.
6. `docs/design/successor-api-stage-setting.md` §2, its lessons L1 to L30: what
   the project already knows scales badly and why.

## 2. Phase A: the gates, in order

Every job as the kit runs its jobs (`common.sh`'s `launch`:
`OSOE_NO_PROGRESS_TIMEOUT=60`, `SHMEM_DEBUG=1`, a timeout and `osoe-stat --gc`
after a kill), on whole nodes (`--exclusive`), as the brief's "The kit on the
site's own Slurm, without `sudo`" says. Two of the kit's habits need a word
here. "Under the site's binding" below means `KIT_SRUN` set to the `--cpu-bind`
and `--cpus-per-task` the site's own jobs use, as the brief's question 6 has
it; "unbound" means the kit's default. And `launch` cleans `/dev/shm` only
after a kill of its own (exit 124 or 137), by an `srun` outside the killed
job's allocation that on a shared cluster may land on other nodes: a step that
Slurm's time limit ends leaves its names on every node, so every allocation
begins with `srun --ntasks-per-node=1 osoe-stat --gc` on its own nodes, and a
`--gc` by hand follows any job that did not end by itself (C19). A gate failed
is recorded - the command, the first error line word for word, the PE and node
that printed it, whether it happened again, and `osoe-stat --summary`'s view
where it hung - and the session goes on to the next gate only where the failure
is understood and does not touch it.

| Gate | What runs | Passes when | On failure |
|---|---|---|---|
| **A0 The site's facts** | `facts.sh` with the brief's additions, on a compute node through `srun`: the kernel and `kernel.io_uring_disabled`; liburing's and CMake's versions; `ulimit -Hn`, `-Hl`, `vm.max_map_count`; `/dev/shm`'s size; the three neighbour-table settings; `net.core.somaxconn`, `net.core.netdev_max_backlog`, `net.ipv4.tcp_rmem`, `tcp_wmem` and `tcp_mem`; the NIC, its speed, MTU and offloads (`ethtool -k`: TSO and GSO, which the Pi's F1 implicated); the subnet shape (`ip -4 route`); **the node names and their addresses**: `scontrol show hostnames` against `SLURM_STEP_NODELIST`'s shape, and for each name the address it resolves to and the interface that address is on - osoe takes a name's first IPv4 address, binds its listeners to every interface and has no setting to choose one (C2); CPUs, cores and NUMA nodes (`lscpu`); Slurm's version, task plugin, the binding its jobs get by default (`Cpus_allowed_list` from a task), its time limits and `RemoveIPC`; whether `ssh` to a node without a job is allowed; `perf_event_paranoid`. And `tools/launch/capture-slurm-fixtures.sh` on a step of two and of four nodes, block and cyclic: the tree's multi-node Slurm fixtures were written from the documentation, never captured from a real cluster | Linux 6.7 or newer with io_uring enabled, liburing 2.6 or newer, CMake 3.25 or newer; names of one bracketed range each, resolving to IPv4 addresses on the fast interface. The rest are recorded, each against the arithmetic of §2.1, and a limit the arithmetic says a planned job will meet is a stop rule for that rung of the ladder, not for the gate | a kernel older than 6.7, or io_uring disabled - which many sites' hardening does, and a container's seccomp profile too: the first lines of the report, and stop - the single-node build (`-DOSOE_SINGLE_NODE=ON`) is all that runs here, one node at a time. **Node names of more than one bracketed range** (`rack[1-4]n[1-8]`, Cray's `x1000c[0-7]s[0-7]b0n[0-1]`): no job of more than one node starts, and there is no setting that mends it under `srun` - the `OSOE_*` variables are read as a whole set only where `OSOE_PROCID` is set, in place of Slurm's - so C2's parser change is this gate's prerequisite, built first with the parser's cases as its tests, or a per-task wrapper sets every `OSOE_*` variable from Slurm's with a one-dimensional `OSOE_STEP_NODELIST`. Names on the wrong interface: `/etc/hosts` or the site's DNS for the fast network's names, as the site allows, and otherwise C2's interface setting before anything is timed |
| **A1 The build** | the `release` preset, with no network, where the nodes' own libraries are; `build/release/tools/probe/probe` on every node through `srun`; `ctest --test-dir build/release -R 'shmem[.](exports\|license-lines\|info)'` (CMake's alternation is `\|` in this table's cell, a plain bar in the shell) | the build without a warning; `ok` for every facility on every node | a facility the probe refuses on some nodes and not others: a heterogeneous cluster, and the job must stay on nodes alike (the README's "Every node of a job must have the same architecture, and run the same osoe"; a heterogeneous Slurm job is refused by name) |
| **A2 One node, the suite** | on one compute node, whole and exclusive - tier 2's jobs share one port range and `/dev/shm`, so another user's osoe job on the node would collide - through an allocation: `ctest --preset release -LE internode`, then `-L internode` - tier 2, the tests of jobs of logical nodes on one machine, every one of which starts its job with `osoe-launch` - at the four helper settings the suite runs (`OSOE_PROGRESS_HELPERS` 0, 2, 3 and unset); then the `debug` and the AddressSanitizer presets' tier 2 once; `OSOE_TEST_FLOOR_PINS=1` on the `release` build, its ratios recorded | every test passes but those the preset leaves out; on one `c4-standard-16` tier 2 passed 1,071 of 1,071 in 148 s (the third cloud session). The floor's pins were set on the project's machines: a pin missed here is a number for the report, not a stop | a test that fails here fails on a machine the project has never had: the first finding, before any job across nodes. Read its output with the link it printed (`OSOE_TEST_PRINT_LINK`), repeat it alone, and record it |
| **A3 Two nodes** | the README's quick start under `srun -N2`; `osoe-bench quick` at 1 and 8 PEs a node; `floor.sh` with `PARTS="floor iperf3"` between two nodes, the yardstick for every stream and round trip after (its `echo-uring` part met its timeout on the cloud and is left out); `osoe-stat --summary` on a live job through `srun --overlap`; a job killed by its timeout and `osoe-stat --gc` after it, inside the allocation | the ring prints its lines; the bench's results check; the floor's ratios recorded against the Pi's (round trips 9 to 11% above an io_uring echo's) and Google Cloud's (a put's fastest round trip 41 µs); `osoe-stat` finds the job | a hang in `shmem_init` on two nodes is the ports (`OSOE_PORT_RANGE`, `OSOE_MESH_PORT_RANGE`, the site's firewall - one that drops a handshake rather than refusing it costs up to 32 s a rendezvous round, a second a candidate port), the names (A0) or `RemoveIPC` (the README's Limits), in that order |
| **A4 The suite's results across machines** | the `cluster` preset (C1, built 2026-11-02): `cmake --preset cluster -DOSOE_CLUSTER_NODES=<nodes> -DOSOE_CLUSTER_PARTITION=<partition>`, built on the shared filesystem, then `ctest --preset cluster` at `OSOE_PROGRESS_HELPERS` 0 and 2, from a login node or inside `salloc -N<nodes> --ntasks-per-node=<P>` - 45 tests, tier 3's programs under `srun` with their registrations' environments (maps of one to thirteen nodes; a test of more than `OSOE_CLUSTER_NODES` is reported disabled), its own `cluster.map.*` first, which must pass before anything else is read, since the uneven maps rest on `srun-map.sh`'s hostfile order; the harness has run in the development VM's one-node Slurm only, so its first cluster run is this gate's first job, and `docs/pi-cluster.md` §4 is its runbook. Then what the harness leaves out, the Pi runbook's way (§7): the test programs themselves under `srun` on two, four and eight nodes - `shmem_test_internode` with the stress settings its registration gives it (`OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2 OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=...`), block and `--distribution=cyclic`; `shmem_test_floor`; `shmem_test_global-exit` and `shmem_test_dead-pe` with `-K0`; and the tier-3 registrations' other programs where their map is one `srun` can give, each with the environment its registration sets. Then `osoe-pacer`'s every pattern but `mix`, each short, at the site's PEs a node - the stress tester checks every result against an oracle, and is the result-checking suite the project has for separate machines; `shmem.quiet.litmus.L2`'s litmus on one node, which x86-64 runs in every preset | every program passes, every pattern `failures=0` | a program or pattern that fails across machines and passes on logical nodes is the finding this gate exists for: a kernel's TCP, a switch or a NIC offload between two processes that one machine never had. Record it with `osoe-stat`'s counters (`transport.am.receive_enobufs`, the retransmissions `nstat` counts) and stop the ladder |
| **A5 Eight nodes, the yardstick** | the kit on eight nodes at 8 and 16 PEs a node, each in three rounds: the start-up probe alone (`kit/startup`, as `scale.sh` runs it - not `scale.sh` whole, which also runs the bench, ten minutes of `mix` and the SOS sweep), `helpers.sh`, `window.sh`, `collect.sh`, `fan-in.sh` and `gets.sh`; `osoe-bench` at 64 and 128 PEs; the `OSOE_LINK_*` each job measured recorded from `SHMEM_DEBUG` | every number set beside the third Google Cloud session's (H11, H12, H26, H28, P37, P38, `docs/fast-link-results.md`) with the two links' speeds beside them; a ratio past 2 either way, after the links' ratio is taken out, is a finding to explain before the ladder - a faster link should make streams faster and round trips not slower | a measurement that varies by more than its spread between rounds is the link's measurement unsteady (P38 found 0.88 to 5.68 GB/s from job to job at 23 Gbit/s): record the three numbers of each job, and set `OSOE_LINK_*` by hand from the floor for the timed runs that follow, saying so |
| **A6 The stress tester at size** | `osoe-pacer mix` at 8 nodes for 600 s, at 64 nodes for 3,600 s, at the whole cluster for 600 s, at the site's PEs a node, two helpers and none; one run of `mix` at the whole cluster for a night where the allocation allows; the planted-defect runs (`tools.pacer.plant.*`) once at 8 nodes, each of which must fail | `failures=0` in every run, and every planted defect caught. The largest so far across machines: 300 s at 32 PEs on eight nodes, an hour at 16 on the Pi | a failure of `mix` is the project's own oracle saying a result was wrong: the ladder stops. Keep the run's `osoe-stat` output and the first line; repeat at the same size once, and at half |
| **A7 The corpus** | at 8 and 64 nodes, built with `oshcc` from the carried-in corpus (`docs/pi-cluster.md` §8): Sandia OpenSHMEM's suite by `sos.sh` (161 of 165 expected, the four SOS's own); OSU's collectives and `osu_oshm_put` and `_get`; ISx weak and strong; NPB IS at class C, D where memory allows; the PRK kernels; the OpenSHMEM examples. If time allows, bale's `histo`, `ig`, `randperm` and `topo`, whose atomics into one array from every PE are the hot-spot pattern of C13 - bale is in the corpus's manifest but no recipe in the tree builds it, and its build is the agent's to work out and record | each program verifies its own result | a program that verifies on 8 nodes and not on 64 is a scale failure of osoe's until shown otherwise; record and stop |

### 2.1 The arithmetic a gate checks the facts against

For a job of n nodes of P PEs each, planned for the ladder's top rung:

| Resource | What a node needs | Where it fails |
|---|---|---|
| Open files (`ulimit -Hn`) | the lead 2(n - 1) sockets and P - 1 descriptors, and the program's own: 8,192 + P + 64 at 4,097 nodes | the start ends naming the limit (`listener.c`); a hard limit of 1,024 runs out at about 385 nodes at 256 PEs a node |
| The neighbour table (`gc_thresh3`) | n - 1 entries on one subnet | the kernel drops packets it cannot address, past about 1,025 nodes at the default; osoe does not check it (C12) |
| `/dev/shm` | P × (`SHMEM_SYMMETRIC_SIZE` + the statics + about 27 MiB) + the control segment (73 MiB at 256 PEs) + the connection area, which the check counts whole though its pages cost only as written (512 KiB a peer: 250 MiB at 500 nodes, 1 GiB at 4,097 once the buffers halve) | refused at `shmem_init` by name: at 256 PEs a node the default heap asks 70.7 GiB, which few nodes' `/dev/shm` hold, so a job at many PEs a node sets `SHMEM_SYMMETRIC_SIZE` lower or `OSOE_SHM_CAPACITY_CHECK=0` |
| `vm.max_map_count` | two maps a node-mate: 2P + the program's | the start ends naming the heap it could not map; 65,530 by the kernel's default holds about 32,000 node-mates |
| Locked memory (`ulimit -Hl`) | the rings: 2.6 MiB at 4,097 nodes on 16 KiB pages at no helpers (a Pi's measurement), a ring a helper more; the zero-copy send's loans besides | a ring refused ends the job by name on Linux 6.14 and later |
| The listen backlog (`somaxconn`) | up to 4,096 callers at a lead | a caller the backlog cannot take waits in TCP's retries, then dials again after a second (T46); 128 on kernels before 5.4, 4,096 since |
| Kernel socket memory | 2(n - 1) sockets a node, each with the kernel's send and receive buffers as traffic sizes them (`tcp_wmem`'s default 16 KiB and `tcp_rmem`'s 128 KiB, the maxima 4 to 6 MiB): at 4,097 nodes, 8,192 sockets, hundreds of megabytes if every one is busy at the defaults, gigabytes where the site raised the maxima for a fast link; `tcp_mem`'s pressure threshold is the node's | not osoe's to check and nothing names it: the node slows as the kernel reclaims; `ss -tm` on a lead shows the buffers, `nstat`'s `TcpExtTCPMemoryPressures` the pressure. Measured by no run yet |
| The lead's private memory | about 8 KB a peer - the control connection's 4 KiB buffer, 64 reply descriptors of 48 bytes, the connection's and wires' state (the README's "Memory", 8.1 KB) - 33 MB at 4,000 nodes, and 1 MiB of receive buffers an owner; every PE 24 bytes a peer and 21 a peer for each context in use: about 180 KB a PE at 4,000 nodes, 46 MB a node of 256 | nothing fails; the figures are for the report's memory table |
| The rank list (`SLURM_GTIDS`) | one environment string of the node's ranks: about 21,000 PEs a node | the launcher cannot start the process |
| CPUs recorded (`OSOE_AGENT_CPUS=node`) | 1,024 | a CPU numbered 1,024 or more is not recorded, and nothing says so |

## 3. Phase B: the ladder

The off-network brief's seven questions are the ladder's core, each with how to
measure it, and are not repeated here. What follows is the frame around them:
the rungs, the battery run at each, the stop rules, and a second ladder in PEs
a node. Three things the kit does not do: it has no sweep over node counts, so
each rung is a node-list file of its own (`NODES`), and the scripts' timeouts,
set at eight nodes (`fan-in.sh` 120 s, `window.sh` 180 s, `KIT_TIMEOUT` 900 s),
rise with the rung; it makes an allocation a job, so a rung of many jobs on a
busy site waits in the queue as many times - the brief's "small change to
`common.sh`" that starts steps inside one allocation is made first, its diff in
the report; and `gets.sh` does not vary the credits, so §3.2's credit cases
set `OSOE_AM_CREDITS` in the environment by hand.

### 3.1 The rungs in nodes

Node counts doubling from 8 to the whole cluster - 8, 16, 32, 64, 128, 256,
512, 1,024, and the whole - at 1 PE a node and at the site's PEs a node, each
rung one allocation, its cases in rounds rotated (`rotate`), the median with the
spread, and the commit, nodes, PEs a node, helpers and every setting that
differed from the defaults in each table. Every timed job of more than a few
PEs a node runs twice: unbound (the kit's default) and under the site's binding
with `OSOE_AGENT_CPUS=node`, since under a binding of a task to one core the
lead's progress threads otherwise share the lead PE's core (C3); a third run,
the site's binding without `node`, at one rung, to show the cost. The battery,
at every rung:

| Measurement | How | What to watch, and the yardstick |
|---|---|---|
| Start-up | the start-up probe (`kit/startup`): PE 0's own `shmem_init`, the slowest and the fastest; each lead's `SHMEM_DEBUG` line of its mesh's time; eager and `on-demand` data wires (the brief's question 1) | 102 to 122 ms the slowest PE at 8 nodes; the model, `mesh_timing.py`, at this cluster's hop; the schedule's floor of ⌈(n - 1)/2⌉ slots of 100 µs, 0.2 s at 4,097; a lead a second late is a dropped handshake (T46) |
| The connections | on a lead, `ss -tn \| wc -l` and `ls /proc/<pid>/fd \| wc -l`; `ip -4 neigh show \| wc -l` and `/proc/net/stat/arp_cache` before and after (question 2) | 2(n - 1) sockets; the neighbour table's `table_fulls` zero |
| Memory a node | `/dev/shm`'s use (`df`) and the lead's RSS (`/proc/<pid>/status`) once the job is up; `ss -tm` on the lead for the sockets' kernel memory | the README's "Memory" table and §2.1's lead figures; the kernel's sockets, which no run has measured |
| The small collectives | `osoe-bench quick barrier latency atomic` in one job, and `collectives` - 8-byte and 64 KiB reduce and broadcast, 4 KiB fcollect, 8-byte and 4 KiB alltoall - in a job of its own, since the bench has no finer switch and its alltoall's 400 trips at the top rungs take hours by the arithmetic below: from 256 nodes, `collectives` gets its own timeout, or the alltoall is left to its row | 117, 161, 434, 337, 480, 469 and 1,283 µs at 64 PEs on eight nodes (H11); a tree of fan-out 8 is one level deeper at 9, 73, 585 and 4,681 nodes, a hop each way more; a small broadcast costs about as many hops as a reduction, its credits going up before its data comes down (C16) |
| The large collectives | `window.sh` with `CASES="link tree"` at 64 KiB to 4 MiB, and Träff's forced by a link set by hand where the chooser does not take it (question 3); the shape each job took, from `SHMEM_DEBUG`, beside `shape_timing.py`'s choice for that job's link; on a link of 50 Gbit/s or more, `fixed-k2-d8` and `-k4-d8` beside them | P37's table at eight nodes; a 1 MiB reduction's time growing with log n, not n; a 4 MiB reduction at a rate well under the link's is the pipeline's cap (C6) |
| `fcollect` and `collect` | `osoe-bench`'s fcollect at 1 KiB a PE; `collect.sh`, every way forced and `link` (question 4) | the exchange's 16 and the room of 64 |
| `alltoall` | `osoe-pacer alltoall` for a fixed count of rounds, or the bench's `collectives` job above, with a timeout from the arithmetic: P(N - P) blocks into a node at about a million messages a second a progress thread - 32.7 million a call at 500 nodes of 256, tens of seconds a call, hours for the bench's 400 | 263 ms for 4 KiB blocks at 512 PEs (H12); the first rung where a collective is unusable |
| Fan-in | `fan-in.sh`, every PE of the other nodes into one, at 64 and 256 receive buffers, 0 and 2 helpers (question 5) | `transport.am.receive_enobufs` beside the rate |
| Streams and round trips | two nodes of the rung's allocation: `osoe-bench`'s put and get streams at 64 KiB and 1 MiB, the 8-byte round trips; the mailbox (`osoe-pacer mailbox`) | the floor from A3; 2,709 MiB/s and 41 µs on Google Cloud; these should not change with the rung, and a change says the node's other n - 2 wires cost it something - the silent-peer check's `getsockopt` on the lead's wires every 10 s among the suspects (C18) |
| The counters | `osoe-stat` on two nodes after the battery: `receive_enobufs`, the deferred reply queues' highest depth, the owners' wakes and passes; `nstat -az TcpRetransSegs TcpExtTCPTimeouts` on a node before and after | retransmissions on an idle switch are a finding (the Pi's F1) |

**Stop rules.** A ceiling met is a result: record its line word for word, go
past it only by the setting the line names, and say so. A hang: `osoe-stat
--summary` from `srun --overlap` on the nodes it names, the deadlock detector's
line if it proved one, the first line, and the rung is repeated once before the
next. A job that takes more than ten times the model's or the smaller rung's
time: stop the ladder at that rung and bisect the setting (helpers, the data
wires, the window, the chunk) before climbing. A wrong result anywhere - the
bench's check, a pattern's oracle, a corpus program's own verification - ends
the ladder until explained. A job that Slurm's time limit ended: `--gc` on its
nodes before anything else runs there (C19).

### 3.2 The rungs in PEs a node

On 8 nodes, by the kit's `many-pes.sh` (§20 of the runbook, written for two
nodes) extended to eight: PEs a node at 16, 32, 64, 128 and the node's cores,
and its hyperthreads as a separate rung, at 0 and 2 helpers, unbound and under
the site's binding, and with `OSOE_AGENT_CPUS=node` (question 6):

| Measurement | Why it is here |
|---|---|
| `shmem_init` and `/dev/shm` at the default heap and at 16 MiB | at 256 PEs a node the default heap's capacity check asks 70.7 GiB; the README says so and no run has met it |
| The barrier and a 64 KiB and 1 MiB reduction | the node's lead combines its PEs' vectors one after another, O(P × bytes) on one thread, and releases them with one wake of every waiter, O(P): at 256 PEs a 1 MiB reduction reads 255 MiB on the lead before a byte leaves the node (C5); H12 measured 1,287 µs for 64 KiB at 256 against 296 at 16 |
| Put and get streams, and the mailbox | one progress thread and two helpers move every byte of 256 PEs, and every put is three locked operations on lines every PE of the node shares (C4): H12's streams fell to a third of the link at 256 and the mailbox was not run there |
| Gets, the landing zone and the credits | the node's 4 MiB budget gives 256 PEs 16 KiB each, under the 256 KiB floor, so the floor holds, 64 MiB of zones; and a node's gets and fetching atomics toward one peer node share that pair's 64 credits (`OSOE_AM_CREDITS`), a quarter of one outstanding a PE at 256 (C11): `gets.sh` at 1 to 256 PEs a node, with `OSOE_AM_CREDITS` 64 and 256 set in its environment, every node alike |
| `shmem_team_split_2d` three and four times | three whole-job grids fill the default pool (`OSOE_TEAM_BLOCKS`) at 256 PEs a node |
| `OSOE_CHECK_COLLECTIVES` on and off | the first barrier's check reads a page of each node-mate's region, and every check is the lead's read of P - 1 regions |
| The agents' placement | `OSOE_AGENT_CPUS` `task` and `node` under the site's binding, each step's `Cpus_allowed_list` recorded; `auto`'s rule - two helpers where the node's PEs are no more than its CPUs online - is true at 256 PEs on a 256-core node, so the lead's core then carries its PE, its agent and two helpers (C3) |

### 3.3 What the ladder does not measure

The zero-copy send (`docs/fast-link.md` §7, off by default since A3), latency
tuning (§15, root) and io_uring's device polling (N3, not built). The kit's
ladder of the transport's settings (§6) is not in the brief, but on a link of
50 Gbit/s or more it is worth one rung of 8 nodes (C6): every default was set
on gigabit and 23 Gbit/s.

## 4. Phase C: where the code is likely to bend

Each item: what the tree has, why it bends at scale, what in phase B would show
it, what the change would be, and its class - a *setting*, a *small change* (a
few dozen lines with its test), a *protocol* (modelled first), or a *design
change* (the author's, described and not made). Ranked by how soon the ladder
meets it: the first two are met before the ladder starts.

**C1 The suite cannot run across machines** - *built, 2026-11-02*
(`docs/decisions/2026-11-02-cluster-tests.md`). Every test of a job started it
with `osoe-launch -n N -m MAP` - 239 registrations in
`shmem/tests/CMakeLists.txt` - and there was no launcher switch, so the suite
that checks results had run on logical nodes of one machine only (the README's
Limits). The `cluster` preset (`OSOE_CLUSTER_TESTS`) now registers the tier-3
list's programs (`osoe_tier3_test`'s 37 registrations, each a map, a program
and an environment) a second time, as steps under `srun`: a block map as `-N<nodes>
--ntasks-per-node=<P>`, a cyclic one with `--distribution=cyclic`, and the
uneven and interleaved maps (`0,0,0,1,2,2`; `0,1,2,0,1,0`, which most of the
list runs on) through `tools/launch/srun-map.sh`, the hostfile
`--distribution=arbitrary` takes; `OSOE_CLUSTER_PARTITION` and
`OSOE_CLUSTER_SRUN_FLAGS` (`-K1 --cpu-bind=none`) are the site's, and
`OSOE_CLUSTER_NODES` disables the maps the site cannot give. The per-rank
environment `osoe-launch -e RANK:NAME=VALUE` gives is
`tools/launch/rank-environment.sh`, keyed on `SLURM_PROCID`, which no tier-3
registration needs. Left to the launcher, and to A4's by-hand runs: the
kill-based tests (`dead-pe.sh`, `death-in-wait.sh`, `-k`), `global-exit.sh`'s
judgement by the launcher's per-rank lines, a link cut inside a namespace, the
differential's transcripts. It has run in the development VM's one-node Slurm
alone (the Pi cluster was unreachable the day it was built): the first cluster
run is A4's.

**C2 Node names, addresses and the interface.** `base/hostlist.c` takes one
bracketed range a name (`node[1-8]`, not `rack[1-4]n[1-8]`), and nothing under
`srun` can give osoe another list: the `OSOE_*` variables, `OSOE_STEP_NODELIST`
among them, are read as a whole set only where `OSOE_PROCID` is set, in place
of Slurm's (`bootstrap.c`). `listener.c` takes a name's first IPv4 address,
binds its listeners to every interface, and has no setting to choose one, so a
site whose node names resolve to the management network carries every byte of
osoe there while the fast network idles; IPv6 is not spoken; a heterogeneous
job and a batch step are refused by name; and nothing but `srun` or
`osoe-launch` can start a job, since the bootstrap reads Slurm's variables or
`OSOE_*`'s and not PMIx. *Shows as*: `shmem_init` ending at A0 naming the list,
or A3's floor at the management network's speed. *Change*: the parser extended
to several ranges a name - a small change with the parser's cases as tests, and
A0's prerequisite on such a site; a setting choosing the interface by name or
subnet - a small change at the listener, a layout change if the rendezvous's
address record widens; IPv6 a design change.

**C3 The progress threads' core.** The lead's agent and its helpers inherit the
lead PE's CPU mask (`agent.c`; `OSOE_AGENT_CPUS=task`, the default): where
Slurm binds each task to one core, the lead's main thread, its agent and every
helper share that core, and `auto`'s rule - two helpers where the node's PEs
are no more than its CPUs online - is true on a node whose every core has a
PE, so a 256-PE node's whole network engine runs on one core beside PE 0's
program. The README says so; H26 measured the helpers at 8 PEs a node of 16
CPUs, never at every core taken. *Shows as*: every stream and collective at the
site's PEs a node slower under its binding than unbound, and the lead's core at
100%. *Change*: `OSOE_AGENT_CPUS=node` in every job at many PEs a node (a
setting, now); then `auto` counting a node's spare CPUs - a rule the author
decided (A2, P36) and would decide again; and a site's way, a core or two left
without a PE, in the README's "Running a program with `srun`".

**C4 One progress thread a node, plus two helpers, against 256 PEs.** Every
internode byte goes through the lead's agent or one of H helpers, the data wire
to the peer in slot p being owner 1 + (p mod H)'s (`owner-map.h`): one thread
copies a peer's every message out of the kernel's buffer into a PE's heap, and
one thread's send is the pair's whole stream. A put is a memcpy into the pair's
shared buffer and three locked operations on lines every PE of the node shares
- the reservation's CAS, the commit's add, the owner's dirty bitmap - then a
load of the owner's sleeping flag and, if it sleeps, a `futex_wake`
(`transport-internode.c`, `connection.c`, `connection-area.h`). Many small puts
to one peer already coalesce: a connection with a send in flight keeps filling
its buffer and the next send carries them all (design §3.9, `progress.c`), with
no threshold, and 32 bytes of wire for an 8-byte put. At 256 PEs a node H12 saw
the streams at a third of the link, the progress threads' CPU a GiB doubled and
`futex_wake` the top symbol, and the mailbox was not run. *Shows as*: the
PEs-a-node ladder's streams and mailbox falling with P at every helper count,
and a hot word's rate (C13). *Change*: C3 first; then helpers by the node's
spare cores; then, as design changes, a sub-buffer a PE or a socket within a
pair's connection so that 256 writers do not share three lines - which rewrites
the model-checked reservation protocol of §3.5 - or the application thread's
own send for a large put, which the single-issuer rings forbid as built.

**C5 The intranode phase on one thread.** A node's lead combines its P members'
vectors one after another into its `dest` before the internode phase (design
§5.5, rule 2 of §5.8; `collectives.c`): O(P × bytes), 255 MiB of reads for a
1 MiB reduction at 256 PEs a node, tens of milliseconds on one core, before a
byte leaves the node - longer than the whole internode phase on a fast link.
Then it releases every member with one `futex_wake` of them all, O(P) kernel
work on its path (the barrier's release is a chain, each woken PE waking two,
O(log P) deep), and with `OSOE_CHECK_COLLECTIVES` on it reads P - 1 members'
call words first. *Shows as*: the PEs-a-node ladder's reductions growing
linearly in P while the broadcasts do not; the barrier's release at 256.
*Change*: a parallel intranode combine - each PE combining one P-th of every
member's vector, a reduce-scatter in shared memory, then the lead's internode
phase on the combined vector, each element still combined once in a fixed order
so that rule 3's bit-identical results hold - a protocol with a model, since the
PEs' arrivals and the lead's reads are two-sided and `MULTIPLE` threads may
lead; a design change for the author. §5 weighs it. *Modelled on 2026-11-02*
(`docs/design/intranode-combine-proposal.md`, `model.intranode_combine`): the
sliced combine holds §5.5's three properties in every case, and a standalone
probe in the four-core VM ran the combine 3.5 to 4 times faster at four
processes; the author decides whether it is built, and when.

**C6 The pipeline's cap on a fast link, and the transport's defaults.** A large
collective's pieces are 16 KiB, a compile-time constant
(`OSOE_INTERNAL_PIECE_BYTES`), and at most 8 are in flight on a tree edge
(`OSOE_COLLECTIVE_PIPELINE`, its highest 8): 128 KiB in flight a round trip of
the loop, which at a 40 µs hop is one to two gigabytes a second an edge - a
tenth of a 100 Gbit/s link, whatever the chooser does. The cost model's closed
form was checked against its event simulation to 31 nodes only, and its tree
cost charges the piece's time and not the message cost on the throughput path.
The transport's own defaults - the chunk 64 KiB, a connection's buffer 256 KiB,
the receive pool 16 KiB × 64 an owner, the unsent limit the link's bandwidth ×
128 µs, a get's pieces 64 KiB at most by a constant (`node.h`) and four in
flight a PE from 16 PEs a node - were set on gigabit and 23 Gbit/s; at 100
Gbit/s the pool is 80 µs of link and the link's measurement streams 1.9 MiB,
150 µs. *Shows as*: a 4 MiB reduction or broadcast at a rate well below the
link's at every rung and every shape; `receive_enobufs` on every stream; the
bandwidth read unsteady. *Change*: the kit's ladder (§6) at 8 nodes on this
link, then the defaults by its table (the author's); the pipeline's capacity
and piece sized from the link's bandwidth-delay product - a layout change,
since the piece sizes every lane, and the author's.

**C7 The link, measured once, from one pair, on a cost model fitted to the Pi,
over a tree that ignores the fabric.** The root's lead times round trips and
streams to its first children at the end of `shmem_init` (`link-measure.c`) -
one TCP stream through the agent's path, not the NIC's capacity - and every node
shapes every large collective from those three numbers. On the cloud the
message cost read 23 to 45 µs by the trimmed mean on a link whose put round
trip is 41 (P38), the bandwidth 0.88 to 5.68 GB/s of a link carrying 2.85, and
the chooser's reductions at 1 and 4 MiB ran 24 to 33% behind a fixed fan-out
of 4 (P37); since 2026-10-28 the message cost is the least of the tries, the
author's decision, timed on no cluster since. Nodes 0 and 1 are usually
neighbours in Slurm's order, so a rack's numbers shape a job that spans the
spine. The tree itself is in level order by node index, so a parent's children
are consecutive nodes but a subtree is scattered over the fabric, and Träff's
first round sends every lead half its vector to the lead n/2 places away - a
bisection pattern priced at one link's bandwidth. *Shows as*: question 3's
tables disagreeing with `shape_timing.py` by more than the spread; the shape
varying from job to job at one size; a 1 MiB reduction's time growing with the
racks spanned, not log n. *Change*: in order - the streams sized to the link's
speed; the costs refitted to this cluster's table (N1's method); then a
per-rack link class and a tree whose subtrees are racks, a design change. A
setting first: the timed runs set `OSOE_LINK_*` by hand from the floor, so that
every job of a table takes one shape.

**C8 `alltoall`: P(N - P) blocks into every node, unscheduled.** Every PE puts
its block to every other PE directly, from the rank after its own round the
team, with `MORE` set, then a barrier (`collectives.c`): a message each up to a
chunk, 24 bytes of header, and for an `alltoalls` with a stride one an element
- 32.7 million messages a call at 500 nodes of 256, every one applied by a
node's one to three progress threads, and a node's 256 PEs starting on
consecutive ranks so that the node's whole output goes to about one node at a
time, throttled by nothing but the unsent queue. *Shows as*: the alltoall's
time at the whole cluster in tens of seconds and `receive_enobufs` climbing;
H12's 263 ms at 512 PEs is the start of that curve, and §3.1 keeps it out of
the battery's job for that reason. *Change*: a two-level alltoall - the node's
P blocks to each destination node packed by its PEs into one message a node
pair, n - 1 messages a node of P² blocks each, unpacked at the receiver in
shared memory - and a pairwise schedule (each node's k-th message to node
(self + k) mod n) so that no link takes more than one stream at a time; for
small blocks Bruck's log₂ n steps, which `fcollect` already has. A design
change; §5. *Modelled on 2026-11-02*
(`docs/design/two-level-alltoall-proposal.md`, `model.alltoall_timing`, a cost
model fitted to the cloud's twenty alltoall points within 1.52): a message a
member a destination node is 6 to 8 times as fast at 8- to 64-byte blocks from
64 PEs a node, under 10% from 4 KiB, where the bytes bind by any algorithm;
Bruck is not worth building; the author decides whether it is built, and when.

**C9 Prefix scans over the team's ranks, not its nodes.** A scan runs over a
pre-order tree of the team's members, every PE a place in it, fan-out 4 and two
pieces in flight under `link` (`internode-generic.c`, design §5.8 "Prefix
scans"): 2P messages, and `scan_timing.py` gives a small scan at 500 nodes of
256 about 13 hops under a block distribution and 45 under a cyclic one, where a
reduction takes 6. *Change*: a hierarchical scan - a scan within each node in
shared memory, a scan of the nodes' totals among the leads, which `collect`'s
places already have as a primitive (`osoe_internode_scan_node_totals`), and
each PE's result from its node's prefix - under a block distribution, which the
design's "runs" already find; a protocol, modelled, a design change.

**C10 `collect`'s lengths under a cyclic or plane distribution** are a vector of
N words, 1 MB a call at 128,000 PEs, through the tree (`collect.c`), and the
exchange of the nodes' totals serves only where a node's PEs are one run of the
team's ranks. *Change*: a per-node vector of its own PEs' lengths exchanged as
the totals are, then each PE's place from its node's prefix and its rank's
order within the node - a design change, small in code.

**C11 A node pair's credits at many PEs a node.** Gets and fetching atomics
take a credit of the pair's `OSOE_AM_CREDITS`, 64 by default, shared by every
PE of the node toward that peer and queued in ticket order
(`transport-internode.c`, `take_credit`); puts take none. At 256 PEs a node a
PE has a quarter of an outstanding get toward a given node, and the reply pool
the lead writes at start is 48 bytes a credit a peer (`progress-host.c`),
12 GiB at the settings' most. *Shows as*: `gets.sh` and the fetching atomics'
rates falling with P at 256 PEs a node while the puts' hold; the credit waits
in `osoe-stat`. *Change*: a setting first (`OSOE_AM_CREDITS=256` or `1024` at
many PEs a node, every node alike, since differing credits end the job); then
a default by the node's PEs, the author's.

**C12 The neighbour table is not checked.** Past about 1,025 nodes on one
subnet the kernel drops what it cannot address and nodes count each other
vanished, and nothing in osoe says why. *Change*: a small one - the lead reads
`gc_thresh3` at start-up and, where the job's nodes on its subnet exceed it,
says so in one line naming the setting; a test by `OSOE_TEST_*` hook. Does not
change the wire.

**C13 Hot-spot atomics.** A remote atomic is a message applied by the owner of
the pair's wire, in arrival order, as a C11 atomic on the target's heap word -
coherent with the node's own CPU atomics - with no combining
(`progress.c`, `rmw.h`); a fetching one takes a credit (C11). Atomics to one
word from every PE of every node - bale's `histo`, a global counter, GUPS -
serialise on at most H + 1 threads of that node, at the message rate of a
thread, about a million a second. *Shows as*: A7's `histo` and `ig` rates per
PE falling with n. *Change*: nothing cheap - combining same-address fetch-adds
within a pass changes the values the callers see, and non-fetching adds could be
combined only by a protocol of their own; the lever is the owners' count (C3,
C4). Measure first.

**C14 Memory at many PEs a node.** Every PE maps every node-mate's heap object:
P × (heap + 27 MiB) of address space a PE, 70.9 GiB at 256 PEs at the default
heap, demand-paged in 4 KiB pages with no huge pages (`OSOE_AM_HUGEPAGES=1`
asks `MAP_HUGETLB` of a `/dev/shm` object, which Linux refuses, so it has no
effect, as the README says; the author retired the setting on 2026-10-11, it
is still in the tree, and on 2026-11-02 he asked for the cost of real
huge-page support - hugetlbfs heaps, or transparent huge pages by `madvise` -
to be studied here before it goes or stays) and no NUMA placement of the heap, the connection
area or the agents. Each process builds its own page tables for the pages it
touches in each mapping: a PE that touches every byte of every heap holds 133
MB of them, a node 34 GB - a worst case, but a dense all-to-all pattern
approaches it. On a two-socket node, half of the owners' copies into the PEs'
heaps cross the sockets, and the connection area's pages land on whichever
socket first wrote them. *Shows as*: the PEs-a-node ladder's barrier and
reductions, and `perf`'s page-walk and remote-DRAM counters where the site
allows it. *Change*: `/dev/shm` mounted `huge=within_size` is the site's and
gives transparent huge pages to everything (a fact for the report); first-touch
by the owning PE and `madvise` are small; NUMA-aware placement of the agents and
helpers is `OSOE_AGENT_CPUS`'s rule; a NUMA-aware intranode tree is C5's.

**C15 Träff's reduction past 512 nodes** is tested only through a lower
ceiling's tests and the cost model; its lane grows a row of 128 KiB a PE for
each doubling of the nodes past 512 (P3); its rounds are 2⌈log₂ n⌉, 24 at
4,096 nodes, a hop each. *Shows as*: question 3's table at 513, 1,024 and the
whole cluster. *Change*: none unless the table says so; the chooser's crossover
is C7's.

**C16 The small collectives' fixed shape.** A barrier, a sync, and a reduction
or broadcast of at most 1 KiB go up a tree of `OSOE_COLLECTIVE_FANOUT`, 8, at
every size and link - three levels to 585 nodes, four to 4,681, a hop each way
a level - and a small broadcast sends its children's credits up the tree before
its data comes down, so it costs about as many hops as a reduction. A level
costs a hop each way and k serial sends at the parent: on the cloud a hop is
about 40 µs and a message's cost read 23 to 45 µs by the trimmed mean, so a
fan-out of 8 pays as much in its parent's sends as in its hop, and a fan-out of
16 (three levels to 4,096 nodes) saves a level and pays eight sends more at
each; 32 saves nothing more below 32,768 nodes. Whether 16 wins turns on the
message cost this cluster measures. *Shows as*: the ladder's barrier against
2⌈log₈ n⌉ hops, and its small broadcast against its small reduction. *Change*:
a setting first (`OSOE_COLLECTIVE_FANOUT=16` at the top rungs, timed against
8); then the small class shaped from the link too, a small change to the
chooser; the broadcast's up pass is a protocol; §5 has the alternatives.

**C17 The mesh, and the kernel's state for it.** A node's lead holds 2(n - 1)
sockets, and the job n(n - 1): 998 and 249,500 at 500 nodes, 8,192 and 16.8
million at 4,097 (`connection-area.c`, `listener.c`, `mesh-schedule.c`). The
control wires stay eager at every setting; only the data wires go on demand
(`OSOE_MESH_DATA_WIRES`). The schedule's model says 4,097 nodes connect in 0.26
to 0.45 s, measured nowhere; and what no model has is the kernel: 8,192
established TCP sockets a node, each with its buffers, its timers,
`OSOE_KEEPALIVE`'s probes (60 s idle, 10 s apart, 6 of them) and a place in the
neighbour table, and a switch that sees 16.8 million flows; the progress pass
itself is O(n/64) bitmap words and visits only the wires with a bit set, so the
pass is not the problem. *Shows as*: start-up off the model's curve at some
rung; `ss -tm` memory and `TcpExtTCPMemoryPressures`, both reads the agent
takes in passing; `table_fulls`; a lead a second late. *Change*: the data wires
on demand by default past some node count (a setting's default, the author's);
then the control wires too, or a control *tree* - the barrier's and the
detector's messages travel the collectives' tree and the detector is a star
from node 0 already, so a lead would hold k + 1 control wires and the data
wires traffic opens - a protocol: the abort's carrier judges the peers by their
control wires (`carrier_model.py`, `control_only`), and the detector's probes
and the silent-peer check would travel a tree; a design change for the author.

**C18 The detector, the liveness check and the idle wakes at thousands of
nodes.** The deadlock detector is a star from node 0 - 4(n - 1) control
messages a round, started only when every PE of node 0 waits; every answering
lead reads `/proc` for its P PEs' threads. The silent-peer check is a
`getsockopt(TCP_INFO)` on every wire of the lead's agent every 10 s - its n - 1
control wires with helpers, every wire without them (`progress.c`,
`progress-host.c`). Every PE's agent wakes every 100 ms (the sweep), 2,560
wakes a second on a node of 256 PEs, and the lead's agent polls P - 1 pidfds at
each. *Shows as*: idle CPU on the leads at the whole cluster; the two-node
streams of 3.1 slower at the top rung than at the bottom; the detector's time
to prove a planted deadlock (`shmem.deadlock.waits.32-nodes` is the largest
test). *Change*: measure first; the check's period, the sweep and
`OSOE_DEADLOCK_SECONDS` are settings; a check that walks the wires a slice at a
time is a small change.

**C19 Small costs that grow with n, and the kit's.** `shmem_pe_quiet` allocates
and zeroes an array of n - 1 flags on every call (`transport-internode.c`, two
places); `shmem_init` reads about fourteen settings for every wire it attaches,
100,000 reads at 4,000 nodes; `shmem_finalize` waits for every peer's goodbye
with no bound but `OSOE_NO_PROGRESS_TIMEOUT`, so one hung node holds every
other's exit until that or the allocation ends (an abort's wait is bounded, 20
s, `OSOE_ABORT_LEAD_WAIT_SECONDS`); and the kit's `osoe-stat --gc` runs only
after a kill of its own, by an `srun` outside the killed job's allocation, so
a step Slurm's time limit ends leaves its names on every node. Each a small
change with its test; the finalize wait a question for the author, since a
timeout that ends a job is a thing the project has refused (design §3.15, "a
fatal timeout by another name"; the decisions index).

**C20 Slurm's time limit, its signals, and the output.** A step that reaches
its time limit gets `SIGTERM` and then `SIGKILL`: osoe has no handler for the
first, a killed PE's node-mates see its death and end the job by name, and the
names in `/dev/shm` stay (C19); nothing in the tree records what a job of
thousands of nodes prints as it dies that way. `SHMEM_DEBUG=1` on every PE
writes a few lines a PE at start and end - the link's, the helpers', the mesh's
- through `srun`'s I/O: at 128,000 PEs hundreds of thousands of lines a job,
and the deadlock detector's or an abort's lines from every node on a failure.
Every kit job is an allocation, so a rung of forty jobs on a busy site is forty
waits in the queue. *Shows as*: the first job killed at its limit, and the
report's size. *Change*: `SHMEM_DEBUG` on PE 0 and the leads only, by a wrapper
keyed on `SLURM_LOCALID` (a kit change); the one-allocation `common.sh`
change the brief names; a note in the README's "How a job ends" on what a
time-limit kill leaves, from what the run showed.

**C21 The fabric osoe does not use.** On an HPC cluster with InfiniBand or
RoCE NICs, osoe's TCP runs over IPoIB or the NIC's Ethernet personality, at the
kernel stack's latency and a fraction of the fabric's bandwidth; the RDMA
backend is designed behind two headers and not built, by the author's decision
(RoCE is a constraint, not work). *Shows as*: A3's floor against the fabric's
own `ib_write_lat` and `_bw`, which the report sets beside osoe's. No change
this playbook asks for; the first discussion point.

## 5. Where a different algorithm might be considered

Each row: what the tree does, the alternative, why it might win here, why it
might not, and how the question is decided - by the project's rule, a model or
a timing simulation first (`shape_timing.py` is the pattern), then a build, then
a measurement, and the author's word before a default moves. The order is the
order of expected gain on a cluster of hundreds of nodes with many PEs a node.
The design notes record one of these as tried and set aside - Rabenseifner's
halving with a fold, for Träff's (2026-10-06); none of the others is named
there.

| Collective | Today | Alternative | Might win because | Might not because | Decided by |
|---|---|---|---|---|---|
| **The intranode reduction** | the lead combines P vectors serially, O(P × bytes), and wakes every member at once | a shared-memory reduce-scatter among the node's PEs: each PE combines one P-th of every vector, O(bytes) a PE, then the lead's internode phase | at 256 PEs a node it takes the 255 MiB of combining off one thread; rule 3 holds since each element is combined once in a fixed order | small reductions gain nothing and pay a sync; the PEs' arrival and the lead's read are a two-sided protocol under `MULTIPLE`; the release's O(P) wake stays | modelled and timed, 2026-11-02 (`intranode-combine-proposal.md`; the model holds, the probe 3.5 to 4 times at four cores); built at the author's word, then the PEs-a-node ladder |
| **`alltoall`** | direct puts, P(N - P) messages into a node, then a barrier | two-level: pack the node's blocks to each destination node into one message a node pair (n - 1 a node), a pairwise schedule (node k's message to node (k + s) mod n in step s), unpack in shared memory; Bruck's for blocks under a few hundred bytes | messages a node fall from P(N - P) to n - 1, 32.7 million to 499 at 500 × 256; one stream a link at a time ends the incast | a copy in and out at each end, and the pack needs scratch of P × P × block bytes a node pair - 64 MiB at 256 PEs and 1 KiB blocks, so it goes in pieces through the lanes | modelled 2026-11-02 (`two-level-alltoall-proposal.md`, `alltoall_timing.py`: 6 to 8 times at small blocks, nothing at 4 KiB, Bruck not worth it); the ladder's alltoall row at 64 nodes, then built at the author's word |
| **The prefix scans** | a pre-order tree over the team's ranks, fan-out 4, two pieces, 2P messages | hierarchical: an intranode scan, a scan of the node totals over the leads (the primitive exists), each PE's result from its node's prefix | about 13 hops become about 6 at 500 × 256 under a block distribution, 45 under a cyclic one; the lanes already carry a scan's pieces | a team whose nodes' PEs are not runs of its ranks needs the per-PE vector (C10's shape); no corpus program calls a scan | `scan_timing.py` extended; a model; the ladder's scan at 1 and P PEs a node |
| **Large broadcasts** | a pipelined tree, fan-out from the link, eight pieces of 16 KiB in flight; an interior lead's link carries k copies | scatter down the tree then Träff's allgather among the leads (van de Geijn's shape), which the reduction's second half already implements | about one vector in and one out at every lead, where the tree's interior sends k; on a link where bandwidth binds, up to k/2 times at large sizes | 2⌈log₂ n⌉ rounds of latency against the tree's ⌈log_k n⌉ levels; at 64 KiB the tree wins, as the reductions' crossover shows; C6's cap binds both | `shape_timing.py`'s closed form for the shape, as Träff's reduction was added; question 3's cases with broadcasts |
| **The barrier** | a k-ary tree of fan-out 8 among the leads, 2(n - 1) messages, 2⌈log₈ n⌉ hops and 8 serial sends a parent a level | a dissemination barrier (⌈log₂ n⌉ rounds, each lead one message out and one in, no root); or the tree at fan-out 16 | no parent sends k messages in turn, which at a message cost near the hop's is half a level's time; no root, so no node's load differs | 12 rounds at 4,096 nodes against the tree's 8 hops, and n⌈log₂ n⌉ messages against 2(n - 1); a lead's control wire to log₂ n peers in turn is the mesh's already | `OSOE_COLLECTIVE_FANOUT=16` timed against 8 at the top rungs; a small-message model with both costs |
| **Small reductions and broadcasts** | the same k-ary tree at fan-out 8, window 1; a small broadcast's credits go up before its data | a binomial tree (⌈log₂ n⌉ hops; a lead sends to at most ⌈log₂ n⌉ children, one a round, the fan-in's serial sends spread over the rounds); recursive doubling for an allreduce (⌈log₂ n⌉ rounds of one exchange, no root, every lead holding the result without the down pass); a small broadcast without the up pass | at a message cost near the hop's, a parent's 8 serial sends are a level's time, and a binomial tree pays one send a round; recursive doubling halves the hops of up and down | more hops than a wide tree where the message cost is small, as on the Pi; recursive doubling costs a round at non-powers of two and n⌈log₂ n⌉ messages; the up pass is what tells a lead its child's slot may be written | the same model, with this cluster's hop and message cost; the up pass a protocol |
| **Large reductions** | Träff's circulant reduce-scatter and allgather where the model says it pays, else a pipelined tree | kept; the question is the crossover on a fast link (C7) and the cap (C6); past 512 nodes a *two-level* Träff's - groups of 64 nodes, then the groups - to cut the rounds from 2⌈log₂ n⌉ | Träff's sends under two vectors a lead at every n; its rounds are 24 at 4,096 nodes, a hop each; Rabenseifner's was modelled and set aside | the lane's rows and grant fields are sized for Träff's as built; a two-level shape is new protocol | the ladder's question 3 first; nothing unless the table says the rounds bind |
| **Topology** | trees in level order by node index, Slurm's order; Träff's partners n/2 apart; one link measured | nodes numbered by rack so that a subtree is a rack (a pre-order tree, as the scans'), and a link class a rack | a reduction's traffic stays in the rack until the top levels; the spine carries log n streams a rack instead of most of the tree's edges | needs the site's topology (`scontrol show topology`, or a name's pattern), which not every site gives; Träff's schedule is circulant and has no racks | the ladder's reductions at rungs that span racks against ones that do not; a model with two link classes |
| **`collect`'s places** | the nodes' totals through the tree, a scan or an exchange to 16 nodes; a vector a PE otherwise | hierarchical under every distribution (C10) | 1 MB a call at 128,000 PEs under `plane` becomes 8 bytes a PE a node, exchanged within the node | the mapping of ranks to places needs the node's order of its PEs, which the rank map has | the ladder's `collect` under `plane=4` |
| **`fcollect`** | Bruck's allgather among the leads, the node's PEs' blocks together | kept | each node receives every other's data once, in ⌈log₂ n⌉ steps | at 4,096 nodes the 12 steps' late messages are n/2 blocks; no better bound exists | nothing; measured on the ladder |
| **Start-up** | the rendezvous tree of fan-out 8, then the mesh by schedule, control wires eager | control wires on demand or along the collectives' tree (C17) | a lead holds k + 1 wires at start instead of n - 1, and the kernel 2(k + 1) sockets a node instead of 2(n - 1) | the detector's and the carrier's views assume a control wire to every node (`carrier_model.py`); the abort's broadcast would travel the tree | question 1's start-up curve; a model of the carrier and the detector over a tree; the author's |
| **Hot-spot atomics** | applied in arrival order by the target's owner threads | none that keeps the semantics; more owners a node (C3, C4) | - | combining changes the values each caller sees | A7's `histo` and GUPS rates; measure |
| **Small puts** | coalesced already: a connection with a send in flight fills its buffer and the next send carries everything queued, no threshold; 32 bytes of wire an 8-byte put; three locked operations a put on the node's shared lines | a sub-buffer a PE, or a socket's PEs, within a pair's connection, merged by the owner; a wire header that batches same-peer puts under one header | the mailbox's 1.25 million a second on eight nodes is bound by the progress thread's cost a message and by the writers' contention, not the link | rewrites the model-checked reservation protocol of §3.5; a batch header changes the wire | the mailbox's rate at the ladder's rungs at 16 to 256 PEs a node; a model of the sub-buffers' merge |

## 6. The campaign, as a checklist, and what the brief's rules say

The brief's rules hold: measure first; change code only where a run cannot go
on without it or a defect shows, each change small with its diff in the file;
a protocol is modelled first, and modelling "is not this session's to start";
a default or a decision in force is the author's. This playbook asks for
more in four places; the author answered on 2026-11-02
(`docs/decisions/2026-11-01.md`), and the grants stand as follows:

- **G1** C1's harness, the suite's cross-node registrations under `srun`: *not
  the cluster agent's* - built here, in the development VM's Slurm, before the
  session (the `cluster` preset, 2026-11-02;
  `docs/decisions/2026-11-02-cluster-tests.md`), so that A4 runs the programs
  under `ctest` from the first day. If the tree given to the session lacks it,
  A4's by-hand runs stand.
- **G2** *granted*: the small changes of C2 (the parser; the interface
  setting), C12 (the neighbour table's check), C19 (`pe_quiet`'s allocation,
  the kit's `--gc`) and C20 (the kit's `SHMEM_DEBUG` on the leads): each a few
  dozen lines with its test, built on the branch where the ladder meets its
  case, not before.
- **G3** *granted*: timing simulations and models in `tools/modelcheck/` for
  the rows of §5 the ladder's numbers favour - written on the cluster, their
  results in the report, nothing built from them there. Four of §4's design
  changes - C5, C8, C9 and C17 - are modelled here before the session, and a
  small-collective cost model for §5's barrier and small-collective rows is
  written here too; the session extends those rather than starting its own.
- **G4** *granted*: the one-allocation change to `common.sh` the brief itself
  names, made before the ladder rather than when the queue forces it.

**Two sessions at least** (the author, answer 14): the first runs the gates and
the ladder's lower rungs, 8 to 64 nodes, and comes back; its findings and G2's
changes are merged here; the second runs the top rungs, the whole-cluster
questions and the PEs-a-node ladder on the tree with them. Each session's
report is the brief's one file.

The campaign, across the two:

1. Carry in the tree with its history, the corpus and liburing's source, as the
   brief says. The brief's branch, `off-network-cluster`, from the commit given;
   the brief's return file, `docs/prompts/off-network-cluster-response.md`,
   with §7's sections added.
2. Phase A's gates A0 to A7 in order; the report's sections 1 to 3 written as
   each passes. On a site with multi-range names, C2's parser change first
   (G2, or the wrapper).
3. Phase B's ladder in nodes (3.1), the brief's seven questions placed on its
   rungs; then the ladder in PEs a node (3.2); the report's tables written as
   each rung ends, so a session that stops early still leaves the account.
4. Phase C: for each of C1 to C21 that the ladder showed, the evidence beside
   the item in the report's "Findings"; the small changes under G2 with their
   tests and diffs inline; the protocols and design changes described as
   questions with the numbers, not made.
5. Under G3, the simulations or models for §5's rows the numbers favour; under
   G1, the harness, and A4's programs under `ctest` through it.
6. The report, by the brief's return contract.

## 7. What comes back

The brief's return contract holds - one Markdown file, self-contained tables,
`<repo>` and `~` for paths, no account names, every code change small with its
diff inline - with these added sections, after the brief's "The tree":

- **The gates**: A0 to A7, each with its outcome, the command and the line that
  decided it; the captured Slurm fixtures' shapes, the names' addresses and
  interface, the binding a task gets.
- **The ladder**: the battery's tables at every rung, in nodes and in PEs a
  node, each beside the yardstick (H11, H12, H26, P37) and the model where one
  exists. Nine rungs, two PE counts and two bindings make about 36 tables of
  nine rows; where the file grows past what one reader reads, the rungs'
  medians go in one table a measurement, the spreads in an appendix.
- **Phase C's evidence**: for each of C1 to C21, met or not, the number that
  says so; and for each of §5's rows reached, what the ladder showed and what
  the model or simulation then said.

## 8. The author's answers

The sixteen discussion points this playbook left, and the author's answers of
2026-11-02, are in `docs/decisions/2026-11-01.md`; this text carries them.
What the session finds that would change a decision in force is a question in
its report, as the brief says, not a change.
