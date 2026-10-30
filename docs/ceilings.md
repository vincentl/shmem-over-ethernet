# Ceilings, and how far each is tested

Every bound in osoe that a job can meet - its value, what happens past it, and
the largest value any run has reached, and where - read from the tree at 0.2.0
on 2026-10-30, after the last change of code an item open before the 0.2 tag
asked for. The README's
[Ceilings, and how far each is tested](../README.md#ceilings-and-how-far-each-is-tested)
gives the ones a user meets, in short; this file has them all, with where in the
source each is set, the field widths no run can approach, and the widest gaps
between a ceiling and what has run. The off-network cluster's brief
(`docs/prompts/off-network-cluster-brief.md`) takes its questions of scale from
here.

**How to read it.** A source is a file under `shmem/src/` unless another
directory is given, with the constant or function that sets the bound, so that
it can be found by name; a record is a file under `docs/`, with the item or
section that holds the run. The largest run is of one of four kinds: a *unit*
test, which checks the arithmetic and starts no job; the registered *suite*,
`ctest`'s tests; *logical* nodes on one machine - `osoe-launch`'s over
loopback, or `tools/launch/tier3.sh`'s network namespaces - as the records give
them, outside the suite; and *separate machines*: eight Google Cloud
`c4-standard-16` over a link of 23 Gbit/s (`fast-link-results.md`) and the
Raspberry Pi cluster's four nodes (`pi-cluster-results.md`). Where nothing has
run near a ceiling, the row says so.

## What has run

The yardstick for every row below:

| Quantity | The suite | Logical nodes on one machine | Separate machines |
|---|---|---|---|
| Nodes in a job | 70 (`shmem.circulant.70`); 66 (`shmem.stat.view.many-nodes`); 64 (`shmem.zero-copy-refusals.64-nodes`) | 254 under `osoe-launch`, a PE a node, and 128 under `tier3.sh` (H23, `decisions/2026-10-19.md`) | 8 on Google Cloud; 4, the Pi cluster |
| PEs on a node | 70 (`shmem.stat.view.many-pes`); 64 (`shmem.barrier.L64`, `shmem.reduce.L64`) | 256 on each of two logical nodes, splits and syncs (round five, `reviews/archive/phase1-step7-slice3-scale-and-performance.md`) | 256 on each of two, streams of puts and five collectives, no gets and no mailbox (H12, `fast-link-results.md`); 64 a node with gets and the mailbox (H12); 32 a node on all eight (H11); 4 on the Pi |
| PEs in a job | 70 | 512, the two logical nodes of 256 | 512 (H12); 256, 32 a node on eight (H11); 16 on the Pi |
| A PE's symmetric heap | 128 MiB (`shmem.stopped-bulk.0-0-1`) | - | 1,500 MiB, `osoe-pacer`'s sort on the Pi (`pi-cluster.md` §6) |
| One transfer | - | - | puts and gets of 64 MiB between two nodes (H9); reductions and broadcasts of 4 MiB on eight (H13, P37) |

## A job's size

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **Nodes a job**, by the connection buffers' budget | 4,097 at the defaults: two buffers to each peer node must fit `OSOE_AM_BUFFER_BUDGET`, 1 GiB (twice the chunk to 64 GiB). The 256 KiB buffers hold to 2,049 nodes and are halved to their floor, twice `OSOE_AM_CHUNK`, from 2,050 (`connection-area.c`, `osoe_am_choose_buffer`) | from 2,050, a line from each node's lead says the buffers were reduced to fit the budget; from 4,098 the start ends, saying to raise the budget or lower the chunk | unit: 2,049, 2,050, 4,097 and 4,098 (`shmem.am-size`); the halving's line on 3 logical nodes at a budget of 1 MiB (`shmem.shrink-notice`); jobs to 254 logical nodes and 8 machines |
| **Data wires a node, on demand** (`OSOE_MESH_DATA_WIRES=on-demand`) | past the budget's reach, the budget over two of the floor's buffers: 4,096 at the defaults, whatever the job's nodes; the control wires stay eager to every node (`connection-area.c`, `osoe_am_data_wires_most`) | the lead says so once at start; a data wire past the count ends the job, naming the node, the count and both settings (`data-wires.c`) | unit: 4,096 at 4,097, 4,098 and 12,288 nodes (`shmem.am-size`); the suite: 4 logical nodes at a budget of 8 KiB, two data wires a node, run and refused (`shmem.data-wires.budget-*`); on no large cluster |
| **Nodes, a lead's io_uring ring** | io_uring's 32,768 submissions, two a wire with a socket: 8,192 nodes at no progress helpers, the lead's ring holding every control and data wire - reached only with the budget raised past its default's 4,097; 12,288 with the data wires on demand; 16,384 with helpers, which take the data wires off the lead's ring (`ring.h`, `OSOE_RING_SUBMISSIONS_MOST`; `ring.c`, `osoe_ring_refuses`) | refused at start by name, with the most nodes at that helper count | unit: each side of 8,192, 12,288 and 16,384 (`shmem.wire.ring-ceiling`); rings for 4,097 nodes made by one lead on a Pi, with no job (`reviews/archive/v0.2-before-part4-refutations.md`); jobs to 254 logical nodes |
| **Nodes, the zero-copy send** (`OSOE_WIRE_ZERO_COPY=on`) | 3,274 at no helpers and the default receive buffers: a ring's completion queue - the receive buffers, two a wire, the 16 notifications a data wire may owe, and two - within io_uring's 65,536; more `OSOE_WIRE_BUFFERS` lowers it, helpers raise it (`wire.c`, `ring_open`) | refused at start by name, saying to turn the send off or lower `OSOE_WIRE_BUFFERS` | 64 logical nodes (`shmem.zero-copy-refusals.64-nodes`) and 8 machines (H8); the refusal itself is not tested |
| **Nodes on one subnet**: the kernel's neighbour table | an entry a peer node, n - 1, against `net.ipv4.neigh.default.gc_thresh3`, 1,024 by default: about 1,025 nodes. Logical nodes in network namespaces of one machine share one table, n(n - 1) entries, more than 1,024 past 32 nodes. The kernel's limit, not osoe's | the kernel drops a packet it cannot address ("neighbor table overflow!" in its log): calls time out and nodes count each other vanished. osoe does not check it; `tier3.sh` refuses by name before it makes a namespace | tier 3: 32 nodes at the default, 64 and 128 with the table raised to 32,768 (`decisions/2026-10-19.md`); 8 machines, seven entries a node |
| **Callers waiting at a lead** | the listen backlog, the job's nodes up to 4,096, cut to the kernel's `net.core.somaxconn` (`listener.c`, `try_listen`); 16 callers at once part-way through the hello, the rest in the backlog (`listener.h`, `OSOE_LISTENER_CALLERS`) | a caller the backlog cannot take waits rather than failing - in TCP's retries, and dialling again after osoe's second (`listener.c`, `PROBE_NS`); before the backlog, the kernel's per-CPU queue of arriving packets, `net.core.netdev_max_backlog`, 1,000 by default, may drop a handshake, which costs that second | 254 logical nodes; at 128 tier-3 nodes, all on one kernel, that queue dropped handshakes and 12 of 30 runs had a lead a second late, none at 65,536 (T46, `decisions/2026-10-20.md`) |
| **PEs a job** | no bound is checked: ranks are 32-bit, `shmem_n_pes` an `int`, and the lock's word keeps a PE in 31 bits (`lock.c`); the design's scale is 500 nodes of 256, 128,000 PEs, computed from the code (the README's "Scaling with nodes, not PEs") | no check of it was found: nothing seen refuses a world past 2³¹ - 1 PEs | 512 (H12) |
| **Nodes in the single-node build** (`-DOSOE_SINGLE_NODE=ON`) | 1 (`transport-single-node.c`) | refused at `shmem_init` by name, before anything is made in `/dev/shm` | the design: its suite leaves out the tests labelled `internode` |

## A node's PEs, and its host

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **PEs a node** | 65,535, a bound kept from the first job map's 16-bit index (`bootstrap.c`, `osoe_bootstrap_read`) | the start ends: "SLURM_GTIDS names N ranks on one node; osoe supports at most 65535" | 256 on each of two machines (H12); 70 on one node in the suite |
| **PEs a node, by the rank list's one string** | the launcher gives a node's ranks as one comma-separated environment string, `SLURM_GTIDS` (or `OSOE_GTIDS`), and Linux holds one string of the environment to 32 pages - 131,072 bytes, its name and `=` among them, on 4 KiB pages, measured in the development VM on 2026-10-30: about 21,000 PEs a node of five-digit ranks, fewer of six | the launcher cannot start the process ("Argument list too long"); not run | nothing near: 256 PEs a node |
| **PEs a node, by the kernel's map count** | each PE maps every node-mate's heap object and, where the program has symmetric statics, their copy too: two maps a node-mate against `vm.max_map_count`, 65,530 by the kernel's default - about 32,000 node-mates - which some distributions raise (1,048,576 on Debian 13) (`node.c`, `map_siblings`). Not checked before the maps are made | a map the kernel refuses ends the start, naming the node-mate's heap it could not map and the system's reason | nothing near: 256 PEs a node |
| **Open files** (`ulimit -Hn`) | a node's lead holds two sockets for each other node - its control wire and its data wire - and a descriptor for each other PE of its node, beside the program's own; `shmem_init` raises the soft limit to the hard one, so a hard limit of 1,024 runs out at about 385 nodes at 256 PEs a node (`init.c`) | the start ends, naming the limit it had and that the hard limit is the one to raise (`listener.c`) | 254 logical nodes, two sockets to each of 253 peers; the refusal at a limit of 32 on 16 logical nodes (`shmem.start-up.descriptors.16-nodes`); the 385 nodes are arithmetic |
| **Locked memory** (`ulimit -l`) | on Linux 6.14 and later io_uring charges every ring to one count of locked memory for all of the user's processes on a host, against each process's limit, whose soft limit `shmem_init` raises to the hard one; a lead's rings took 128 KiB at 254 nodes and 2,592 KiB at 4,097 on a Pi's 16 KiB pages, at no helpers (`reviews/archive/v0.2-before-part4-refutations.md`); the zero-copy send charges what it lends besides (`init.c`, `ring.c`) | a ring the limit refuses ends the job at start, naming the limit; a zero-copy send refused goes again by copy, and that connection copies from then on, with one notice (`progress.c`) | 254 logical nodes on one Pi; 64 logical nodes, every process held to 8 MiB, refusals on every node in 20 of 20 runs and every byte right (`shmem.zero-copy-refusals.64-nodes`) |
| **`/dev/shm`** (`OSOE_SHM_CAPACITY_CHECK`) | the capacity check asks for P × (`SHMEM_SYMMETRIC_SIZE` + the program's statics + the internal region, 25.3 MiB at 4 nodes to 26.5 at 500) and the control segment - 70.7 GiB a node of 256 PEs on 500 nodes at the default heap, though pages cost only as written - and the connection area checks its own, 250 MiB at 500 nodes (`node.c`, `connection-area.c`) | refused at `shmem_init` by name, with what to lower; `OSOE_SHM_CAPACITY_CHECK=0` warns and goes on | the suite sizes requests from the free space (`shmem.dev-shm-capacity.*`); 256 PEs a node on a heap of 16 MiB (H12); the default heap at 256 PEs a node has not run |
| **CPUs recorded for `OSOE_AGENT_CPUS=node`** | 1,024: a task's CPUs are recorded as glibc's `cpu_set_t` holds them, `CPU_SETSIZE` (`node.h`, `OSOE_TASK_CPU_WORDS`; `agent.c`, `run_on_node_cpus`) | a CPU numbered 1,024 or more is not recorded, so `node` places no agent on it, and nothing says so | no machine of that many CPUs has run it |

## Teams, contexts and threads

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **Teams a PE is in** (`OSOE_MAX_TEAMS`) | `4` to `1024`, `256` by default, the three predefined teams among them (`bootstrap.h`) | a split returns nonzero on every PE of its parent, and its rank 0 says why, naming the setting (`team.c`) | 1,021 teams held at once at 1,024 on 4 PEs with `OSOE_TEAM_BLOCKS=4096` (round five's conformance, `reviews/archive/phase1-step7-slice3-conformance.md`); the suite's exhaustion at 4 and 8, its agreement at 128; 12 numbers on the Pi |
| **Team blocks a node holds** (`OSOE_TEAM_BLOCKS`) | `8` to `65536`, `1024` by default, a kilobyte each of `/dev/shm` on aarch64 and 640 bytes on x86-64; a team of one PE takes none. Three whole-job `shmem_team_split_2d` grids fill the default at 256 PEs a node (`bootstrap.h`) | the split returns nonzero on every PE of its parent; each PE whose claim failed says so, naming the setting and the node (`team.c`) | 8,192 blocks at 256 PEs on each of two logical nodes, six grids before the seventh ran out of team numbers at `OSOE_MAX_TEAMS=16` (round five's scale review); 1,024 in the suite (`shmem.team-claim.1024`); 65,536 only for its memory |
| **Contexts a PE has at once** (`OSOE_MAX_CONTEXTS`) | `1` to `4096`, `64` by default, the default context among them (`bootstrap.h`) | `shmem_ctx_create` returns nonzero and the PE says so once, naming the setting (`context.c`); at 4,096 a thread place's block lies past the 4,095 a get's token can name, so its blocking gets of more than a piece go a piece at a time | 4,096 on 2 logical nodes (`shmem.get-pieces.fallback.0-1`); exhaustion at 1 and 3 |
| **Live threads of a PE that have called the library** (`OSOE_MAX_THREADS`) | `1` to `1023`, `16` by default, the main thread among them; a thread keeps its place until it exits (`node.h`, `OSOE_MAX_THREADS_HIGHEST`) | the job ends, naming the setting and its most | 21, twenty workers and the main thread at `OSOE_MAX_THREADS=21` (round four's conformance, `reviews/archive/phase1-release-conformance.md`); the suite at 1, 2 and 8; 1,023 accepted at start |
| **Lanes a PE has** (`OSOE_MAX_THREADS` + 1) | a lane a thread place, 1,153 KiB of address space at the defaults to 512 nodes, 128 KiB more for each doubling past it: 19.1 MiB a PE (`internal-region.h`) | the `/dev/shm` check refuses at `shmem_init` what does not fit | the default's 17 everywhere |

## Memory and sizes

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **A PE's symmetric heap** (`SHMEM_SYMMETRIC_SIZE`) | above 0 and at most 4 TiB, the watch word's 40-bit offset of 4-byte grains; `256m` by default (`watch-range.h`, `OSOE_WATCH_MAX_REGION`; `bootstrap.c`) | `0` and above 4 TiB are refused by name; PEs that differ end the job by name | 1,500 MiB a PE on the Pi; 128 MiB in the suite |
| **`shmem_align`'s alignment** | to 4 GiB: every heap base the library tries is a multiple of 4 GiB (`node.c`, `heap_base_candidates`), and under `OSOE_HEAP_BASE` the address's own alignment | returns NULL on every PE, a result the specification defines (`malloc.c`, `pshmem_align`) | 4,096 in the suite (`test-allocator.c`, `test-heap.c`) |
| **A get's landing zone** (`OSOE_GET_LANDING_BYTES`) | a multiple of 1 KiB from 1 KiB to 65,535 KiB; unset, the node's 4 MiB split among its PEs, at least 256 KiB and at most 1 MiB a PE (`am-configuration.h`) | a new get waits for earlier ones to land: back-pressure, never an error | 16 MiB a PE at 8 PEs a node on eight machines (H9); the default split to 256 PEs a node (H12) |
| **The watch word's range** | a range above 16 MiB saturates to the rest of its region (`watch-range.h`) | a wait over-wakes; it never misses a wake | `shmem.watch-range`, every case by brute force |

## The transport

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **A message** (`OSOE_AM_CHUNK`) | a power of two from 1 KiB to 1 MiB, 64 KiB by default; larger transfers are split, a put's part no smaller than a sixteenth of the chunk (`am-configuration.h`) | refused by name | 1 KiB in many tests; 16 and 32 KiB on the Pi; nothing above the default |
| **A connection's buffer** (`OSOE_AM_BUFFER`) | a power of two from twice the chunk to 1 GiB, four chunks by default; shrunk by the budget (above) (`am-configuration.c`) | refused by name | 2 KiB to 1 MiB in the suite, 1 GiB only to provoke the `/dev/shm` refusal; 4 MiB on eight machines (H5) |
| **The buffers' budget** (`OSOE_AM_BUFFER_BUDGET`) | twice the chunk to 64 GiB, 1 GiB by default (`am-configuration.c`) | refused by name | 8 KiB and 1 MiB in the suite, 64 GiB only to provoke the `/dev/shm` refusal; nothing above the default ran a job |
| **A connection's credits** (`OSOE_AM_CREDITS`) | `1` to `65536`, `64` by default (`am-configuration.h`) | a request waits for a credit: back-pressure | 1 and 2 in the suite; nothing above the default |
| **The reply pool** | a node's lead holds, written whole at start, a descriptor of 48 bytes for each credit of each data wire and one more a peer node, and (`OSOE_COLLECTIVE_FANOUT` + 1) × `OSOE_TEAM_BLOCKS` for the collectives' forwards - 9,216 of those, 432 KiB, at the defaults; the settings at their most, 65,536 credits and 4,096 peers, would ask about 12 GiB of each lead (`progress-host.c`, `descriptors_of`; `reply.h`) | no program can empty it while every node has the same `OSOE_AM_CREDITS`: a pool that runs dry ends the job, naming both nodes and the queues (`progress.c`, `pool_exhausted`) | the dry pool's end, by credits that differ (`shmem.reply-pool.overflow`); nothing above the default's 64 credits |
| **The receive pool** (`OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS`) | 64 bytes to 1 MiB, and a power of two from 2 to 32,768 buffers: 16 KiB × 64 by default, 1 MiB for each owner of a node's wires - its lead's agent and each helper - however many peers send to it (`wire-settings.h`) | out of range, refused by name. A pool that runs dry is not an error: the kernel ends the wire's receive with `ENOBUFS`, the owner arms it again once the pass has emptied its buffers, and `transport.am.receive_enobufs` counts it (`ring.c`, `received`) - with no notice, so only the counters show it | 64 bytes and 2 buffers in the suite; 16 KiB × 256 and 64 KiB × 64 on eight machines (H5); a fan-in of seven nodes into one (H10); at many peers, not measured (A6, `decisions/2026-10-20.md`) |
| **Unsent bytes on a data wire** (`OSOE_WIRE_UNSENT_BYTES`) | `0`, the system's own, or up to 1 GiB; unset, the link's bandwidth × 128 µs, at least 16 KiB (`wire-settings.c`) | above 1 GiB refused by name | 2,000,000,000 refused in the suite; 4 MiB, the rule's 417 KB and the system's own, which sets none, on eight machines (H5) |
| **The zero-copy send's notifications** | 16 a data wire may owe, one of a connection's two buffers lent at a time (`wire-settings.h`, `OSOE_WIRE_ZERO_COPY_OWED_MOST`) | past them a send copies | the suite at 64-byte sends; eight machines (H8) |
| **Progress helpers** (`OSOE_PROGRESS_HELPERS`) | `0` to `64`, or `auto`, the default: two on three nodes or more where a node's PEs fit its CPUs, none otherwise (`bootstrap.h`) | above 64 refused by name; helpers beyond a node's peers sit idle with their rings | 64 on two and on four logical nodes, 16 on eight under a stream (round eight's readiness, `reviews/archive/v0.2-step4-readiness.md`); 0, 1, 2 and 4 on eight machines (H4) |

## Collectives

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **The tree's fan-out** (`OSOE_COLLECTIVE_FANOUT`) | `2` to `32`, `8` by default; it sizes every lane and the tree's arrays (`collective-tree.h`, `bootstrap.c`) | refused by name | 32 on 3 logical nodes alone, where a tree has two children at most (`shmem.circulant.3.window-128`); 8 on eight machines, trees of seven children; a fan-out of 32 has not met a team of 33 nodes |
| **Pieces in flight** (`OSOE_COLLECTIVE_PIPELINE`) | `1` to `8`, `8` by default since 2026-10-28 (`bootstrap.h`) | refused by name | 8 on eight machines (P37) |
| **A small collective** (`OSOE_SMALL_COLLECTIVE_BYTES`) | `128` to `16384`, `1024` by default; a large call goes in pieces of 16 KiB (`bootstrap.h`, `internal-region.h`) | refused by name | 128 in the suite; 16,384 never set |
| **Träff's reduction, nodes in a team** | 65,536: a lane has the ceiling's + 1 rows or the ⌈log₂ n⌉ the rounds need, whichever is more, to the reduce-scatter's 16 rows (`internal-region.h`, `collective-shape.c`) | the chooser takes a tree; nothing is refused | unit: the arithmetic at every count to 70 and the grant fields beside every power of two to 65,536 (`shmem.circulant.arithmetic`); 70 logical nodes (`shmem.circulant.70`); 8 machines, where the chooser took it; 257 to 512 nodes only through a lower ceiling's tests, and past 512 only the cost model |
| **`collect`'s exchange of the nodes' totals** (`OSOE_COLLECTIVE_PLACES`) | room for 64 nodes; the default takes it to 16 (P35), past which the tree or the scan (`internal-region.h`, `OSOE_INTERNAL_EXCHANGE_NODES_MOST`; `collective-shape.h`, `OSOE_PLACES_EXCHANGE_DEFAULT_NODES_MOST`) | a forced `exchange` on a team past its room falls back to `totals` | 17 logical nodes in the suite; the fastest of the four ways at 8 and 16 logical nodes (H24); 8 machines at 8 to 32 PEs a node (H25); past eight machines the 16 is untested |
| **No ceiling, a cost** | `alltoall` sends P(N - P) blocks into a node; `collect`'s lengths are a vector of N words where a node's PEs are not one run of the team's ranks, 1 MB a call at 128,000 PEs; a scan runs over a tree of the team's ranks | - | an `alltoall` of 4 KiB blocks at 512 PEs, 263 ms (H12) |

## Launching, and settings that bound no capacity

| Ceiling | Value, and where it is set | Past it | Largest run, and where |
|---|---|---|---|
| **Rendezvous ports** (`OSOE_PORT_RANGE`, `17000-26999` by default, within 1024 to 65535) | a node tries 32 candidate ports of the range, drawn from the job's ids; one host's logical nodes share it (`listener.c`, `CANDIDATES`) | the start ends: "node N found none of its 32 candidate ports free in OSOE_PORT_RANGE=L-H; choose a range this host does not use" | 254 logical nodes on one host; 40 starts in a row on two nodes (`shmem.init.40-sessions.0-1`); the refusal is not tested |
| **The mesh's ports** (`OSOE_MESH_PORT_RANGE`) | unset, a port the kernel chooses; set, any free port of the range, from one the node's ids pick (`listener.c`) | the start ends naming the range | a range of one port refused on 2 nodes; 3 leads in a range of 100 |
| **The mesh's schedule** (`OSOE_MESH_SLOT`) | `0` to `100000` µs a slot, `100` by default; ⌈(n - 1)/2⌉ slots, at most eight calls in flight (`mesh-schedule.h`, `bootstrap.c`) | out of range, refused by name; calls slower than a slot's share set the pace | unit: the schedule at every count to 300 and either side of 512, 1,024, 2,048, 4,096 and 8,192 (`test-mesh-schedule.c`); a mesh of 254 logical nodes in 0.65 to 0.75 s, of 128 on tier 3 (H23); at 4,097 only the model, `tools/modelcheck/mesh_timing.py` |
| **The rendezvous's tree** | a fan-out of 8, ⌈log₈ n⌉ levels (`rendezvous.h`, `OSOE_RENDEZVOUS_FANOUT`) | - | 254 logical nodes, with and without `OSOE_JOB_SECRET` (round four's scale review, `reviews/archive/phase1-release-fixes-scale-and-performance.md`) |
| **A host list** (`SLURM_STEP_NODELIST`) | one bracketed range a name: `node[1-8]`, not `rack[1-4]n[1-8]` (`base/hostlist.c`); and a name must resolve to an IPv4 address (`listener.c`, `osoe_listener_node_address`) | `shmem_init` ends, naming the list: "more than one bracketed range in a name (multi-dimensional names are not supported)"; a name that resolves only to IPv6, saying it cannot be resolved | `base.hostlist`, the parser's cases |
| **`OSOE_KEEPALIVE`** | `IDLE,INTERVAL,COUNT`: 1 to 32,767 s, 1 to 32,767 s and 1 to 127 probes, or `0` (`bootstrap.c`) | refused by name | the default and the tests' values |
| **Timing settings** | `OSOE_DEADLOCK_SECONDS` `0` to `86400` (`deadlock.c`); `OSOE_AGENT_SLICE_US` `0`, or `100` to `100000` (`agent.c`); `OSOE_NO_PROGRESS_TIMEOUT` any 32-bit count of seconds (`bootstrap.c`); `OSOE_LINK_HOP_NS` to 10¹⁰, `OSOE_LINK_MESSAGE_NS` to 10⁹, `OSOE_LINK_BYTES_PER_SECOND` to 10¹³, all three or none (`bootstrap.h`) | refused by name | the defaults and the tests' values |
| **`osoe-launch`** | `-n` 1 to 1,024 processes; 254 logical nodes without a host list, one loopback address each, 127.0.0.1 to .254; 64 `-e` overrides; `-t` 1 to 86,400 s (`tools/launch/launch.c`) | refused by name | 254 logical nodes; 512 processes |
| **`tier3.sh`** | 254 nodes, one /24; the neighbour table above (`tools/launch/tier3.sh`) | refused by name | 128 nodes |

## Field widths no run approaches

Each is set by a layout's field, and the code checks it, refuses past it or
lets it wrap where both sides wrap alike: the abort word keeps a rank in 32
bits and an exit status in eight (`node.h`); a get's landing token, its extent
in 16 bits, its context block in 12 (the contexts' row above) and a sequence
that wraps in 31 (`node.h`); the lock's word, a PE + 1 in 31 bits (`lock.c`); a
team handle's 64-bit generation, about 2⁴⁵ takes of one number at the most
numbers, past which the take ends the job by name (`team.c`); the credit word's
three 21-bit fields, tickets modulo 2²¹ with at most one outstanding a local PE
(`reply.c`); `OSOE_CHECK_QUIET`'s stamps, a 24-bit issue number beside a 40-bit
clock (`transport-internode.c`); Träff's piece number, its round in 5 bits and
its index in 27, alike on both sides (`internode-generic.c`); and the fixed
names and buffers, a blocked routine's name in 32 bytes, an object's in 96, an
abortive goodbye in 64 and `SHMEM_MAX_NAME_LEN`, 256, each held at build time
by a static assertion where it must.

## The widest gaps

Each ceiling a job can meet over the largest value of it any run reached, the
separate machines' figure beside it where it differs; the field widths above
are left out, as is the size of a job in PEs, which nothing bounds (512 have
run of the 128,000 the design is computed for).

1. **A PE's heap**: 4 TiB against 1,500 MiB on the Pi, about 2,800 times (the
   suite, 128 MiB).
2. **A connection's credits**: 65,536 against the default's 64, 1,024 times;
   the tests lower it, and nothing raises it.
3. **Träff's nodes in a team**: 65,536 against 70 logical nodes, 936 times
   (8,192 times against eight machines).
4. **PEs a node**: 65,535 against 256, 256 times - but the rank list's one
   string stops at about 21,000.
5. **A connection's buffer** and **unsent bytes on a data wire**: 1 GiB each
   against 4 MiB, 256 times.
6. **The neighbour table on a flat network**: 1,024 entries against seven on
   separate machines, 146 times; tier 3 reached 16,256 only with the table
   raised.
7. **Receive buffers**: 32,768 against 256, 128 times.
8. **The buffers' budget**, 64 GiB against its default, and **a lead's ring**,
   16,384 nodes against 254 logical: 64 times each.
9. **The zero-copy send's nodes**: 3,274 against 64 logical, 51 times (409
   against eight machines).
10. **Live threads**: 1,023 against 21, 49 times.

Against separate machines alone - eight nodes, 256 PEs on each of two, 512 in a
job - the nodes' ceiling, 4,097, is 512 times what has run, a lead's ring 2,048
times, Träff's 8,192 times and `OSOE_MAX_TEAMS` 85 times the Pi's 12.

**What no run has gone past.** A heap of 1,500 MiB; a connection's buffer of 4
MiB, the buffers' budget of 1 GiB, a chunk of 64 KiB, 64 credits, receive
buffers of 64 KiB or 256 of them; 254 logical nodes or 8 separate ones, so that
no job has met the budget's 4,097 nodes, the ring's 8,192 to 16,384 or the
zero-copy send's 3,274, whose arithmetic only the unit tests check, nor the
backlog's 4,096, Träff's 65,536 or a flat network's neighbour table at about
1,025; 256 PEs a node and 512 in a job; 21 live threads; teams in a pool of
more than 8,192 team blocks; a small collective above 2,048 bytes; a fan-out of
32 on a team of more than three nodes. And four refusals no test reaches: the
zero-copy send's past 3,274 nodes, `tier3.sh`'s for the neighbour table, every
one of a node's 32 candidate ports taken, and a rank list too long to start.