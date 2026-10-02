# osoe

**OpenSHMEM over Ethernet**: an implementation of the OpenSHMEM 1.6
specification for clusters connected by ordinary Ethernet. It needs no RDMA
hardware and no fabric library. It builds from a Linux distribution's own
packages - a C compiler, CMake, pkg-config and liburing - and carries every
operation between nodes over TCP, driven by io_uring. It was developed on a
cluster of Raspberry Pi 5 boards and designed to stay usable at hundreds of
nodes with 256 processes each.

**Status.** Every routine of OpenSHMEM 1.6's C interface is implemented, the
deprecated ones of its Annex F included. What departs from the
specification, or takes a choice it leaves open, is listed under
[Limits](#limits-and-where-osoe-departs-from-the-specification). osoe has not
made a release yet: there is no version tag, and it is built and tested from
the repository as it stands.

## OpenSHMEM in two minutes

OpenSHMEM is a library for SPMD parallel programs, like MPI: a launcher starts
N copies of one program, the *processing elements* (PEs), numbered 0 to N - 1.
What differs is how they communicate.

- **One-sided.** Every PE allocates the same objects, in the same order, from
  its *symmetric heap* (`shmem_malloc` is collective), so the address of its
  own copy names the matching object on every PE. `shmem_put(dest, source, n,
  pe)` writes into PE `pe`'s copy of `dest`, and `shmem_get` reads from it:
  the target takes no part, and is not interrupted.
- **Atomics** - fetch-and-add, compare-and-swap, swap, and, or, xor - update a
  word on any PE.
- **Completion and ordering are explicit.** A put may still be in flight when
  it returns: `shmem_quiet` waits until every put this PE issued has landed,
  and `shmem_fence` keeps puts to one PE in order. A barrier does both and
  synchronizes.
- **Signals and waits**: put-with-signal delivers data and then sets a flag in
  one call; `shmem_wait_until` sleeps until a local word changes.
- **Collectives** - barrier, broadcast, reductions, collect, alltoall, prefix
  scans - run on *teams*: the world, the PEs that share a node, or subsets made
  by splitting a team.
- **Contexts** are independent streams of operations, so that threads can
  issue and complete their own without waiting for each other's.

A ring, each PE writing its number into the next PE's box:

```c
#include <shmem.h>
#include <stdio.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();

    long *box = shmem_malloc(sizeof *box);  /* collective: every PE gets one */
    *box = -1;
    shmem_barrier_all();                    /* every PE's box is ready */

    shmem_long_p(box, me, (me + 1) % npes); /* write into the next PE's box */
    shmem_barrier_all();                    /* completes the writes, then syncs */

    printf("PE %d of %d: my box holds %ld\n", me, npes, *box);
    shmem_free(box);
    shmem_finalize();
    return 0;
}
```

```sh
oshcc -O2 -o ring ring.c
srun -N2 --ntasks-per-node=4 ./ring
```

## Getting started

### Requirements

- Linux 6.7 or newer
- liburing 2.6 or newer
- CMake 3.25 or newer, pkg-config, and a C11 compiler

- Python 3, optionally: the model checker and the stress tester's counting
  build use it. Where CMake does not find it, it says so while configuring and
  leaves those out; nothing else needs it.
Debian 13 (Trixie) and Raspberry Pi OS Trixie provide all of these from their
own repositories. Older releases are not supported, even where the kernel is
new enough: see the design notes, §1.

### Building

Builds use CMake presets and run on Linux only. Each preset builds under
`build/<preset>`, which git ignores:

```sh
cmake --workflow --preset release    # configure, build, and test
```

Presets: `debug`, `release` (optimized, with debug info), `asan`
(AddressSanitizer and UndefinedBehaviorSanitizer), and `tsan`
(ThreadSanitizer).

The `env.probe` test checks that the running kernel provides every io_uring and
futex facility osoe relies on. If it fails, nothing else will work on that
machine.

### Developing on macOS

io_uring is Linux-only, so development on a Mac happens in a Lima VM defined in
this repository. The presets are disabled on macOS and refuse to run there.

```sh
brew install lima
dev/lima/create.sh                                 # Debian 13 VM named osoe
limactl shell osoe cmake --workflow --preset debug # run from the repository root
```

The VM mounts the repository at the same path it has on the Mac, so commands run
from the repository root start in the same directory inside the VM, and
`build/debug/compile_commands.json` works with an editor on the Mac.

### Installing

```sh
cmake --install build/release --prefix /opt/osoe
```

installs the library, its headers - `shmem.h`, `pshmem.h`, `shmemx.h`, and the
`mpp/` forms older programs include - `oshcc`, `osoe-stat` and `osoe-pacer`:
all that a node running jobs needs, and a check that osoe works there. The
installed programs find the rest from where they are, so the prefix may be
chosen at install time and a package may stage it elsewhere. `osoe-launch` and
`osoe-bench` stay in the build tree: they are for development, tests and
measurement.

Once installed, run the stress tester's smoke test under the cluster's launcher:

```sh
srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 /opt/osoe/bin/osoe-pacer --quick
```

It runs its exercises at small sizes, checks every result, and ends with
`osoe-pacer failures=0` and exit status 0 - in 7 s on the Pi cluster's 16 PEs.
`osoe-pacer --help` lists what else it runs.

### Building a program

`oshcc` compiles and links an OpenSHMEM program against osoe, as other
implementations' wrappers do, so build systems that expect one work unchanged
(`CC=oshcc`, `SHMEMCC=oshcc`). It is `build/<preset>/tools/wrapper/oshcc` in a
build tree and `bin/oshcc` once installed:

```sh
oshcc -O2 -o my_program my_program.c
oshcc -show -o my_program my_program.c    # print the command instead
```

It adds osoe's headers, and when it links a program, the library, a run path to
it, and what makes the program's global and static variables symmetric:
`-fno-pie`, `-no-pie` and osoe's linker script (design §4.3). A program linked
some other way runs, but its statics are not symmetric, and passing one where a
symmetric address is required ends the job with a message saying so. `OSOE_CC`
names another C compiler.

### Running a program with `srun`

```sh
srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 ./my_program
```

No PMI plugin is involved: each node's lead finds the others over TCP, on a
port it derives from the job, step and node ids (`OSOE_PORT_RANGE`, default
17000-26999), and then connects to each on a port the kernel chooses, so TCP
must be open between the nodes, not only that range. Every node must resolve
the names in the step's node list, and the program must be at the same path on
every node.

- **`-K1`** (or `KillOnBadExit=1`), so that a task that fails at start-up ends
  the rest rather than leaving them waiting (design §3.15).
- **`--cpu-bind=none`**, or two CPUs a task (`-c 2`) where there are cores to
  spare; `SLURM_CPU_BIND=none` in the environment does the same. Where a node
  runs as many tasks as it has cores, Slurm binds each task to one core by
  default, and the thread that moves every message of the node - the progress
  thread of its first PE - is bound with that PE: it waits behind the PE's own
  thread while the other cores idle. On the Pi cluster that cost a pipelined
  point-to-point pattern six times its rate; the short scheduler slice osoe
  asks for that thread (`OSOE_AGENT_SLICE_US`, below) makes it four, and only
  unbinding the tasks recovers the rest.
- **`OSOE_NO_PROGRESS_TIMEOUT=60`** while a cluster is new, so that a wait on
  the library's own progress - a connection's acknowledgements, buffer space,
  a reply - that stops moving ends the job and says what it waited for.
- **`OSOE_JOB_SECRET`**, 32 hexadecimal digits exported before `srun`, keeps
  other hosts out of the job's rendezvous (design §6.2).

A program run straight from a batch script, not through `srun`, is refused,
since it is no task of a step.

### Running a program on one machine, with `osoe-launch`

`build/<preset>/tools/launch/osoe-launch` starts the processes of a job on this
machine and sets every bootstrap variable for them:

```sh
build/release/tools/launch/osoe-launch -n 4 -- ./my_program
build/release/tools/launch/osoe-launch -n 6 -m 0,0,1,1,2,2 -- ./my_program
```

`-m` is a logical node map (design §8.1): one node id per rank, each node's
lead listening on its own loopback address, so one machine runs a job of
several nodes, over TCP between them as a cluster would - up to 1,024
processes on 254 logical nodes. `-s` names the variables as `srun` does, so
the job takes the bootstrap's Slurm path.

It fails if any process fails, if the timeout (`-t SECONDS`, default 60)
expires, or if the job leaves shared memory behind. `-e RANK:NAME=VALUE` sets a
variable for one rank only. `-k` leaves the surviving processes alone when one
fails, instead of terminating them, so that what ends the job is the library's
own reaction rather than the launcher's.

### Looking inside a job

`osoe-stat` reads a running or hung job's counters and state from its node's
control segment, without the job's cooperation:

```sh
osoe-stat 1234            # job 1234 on this node: counters, and where each PE waits
osoe-stat --summary 1234  # each team's collective in progress, to walk a hang down
```

A job that hangs shows, for every PE, the routine it is blocked in, the
collective's epoch, and for how long. A job that deadlocks - every PE waiting
in the library, with nothing in flight between its nodes - ends by itself after
`OSOE_DEADLOCK_SECONDS` and prints the same, node by node (below); a hang the
library cannot prove, such as a PE computing for ever, is what `osoe-stat` is
for.

## Design goals

- **Commodity Ethernet, no fabric offload.** Every operation between nodes is
  carried by the host CPU over TCP. The design notes judge performance against
  a local floor - a bare io_uring TCP echo, a stream, a memcpy - rather than
  against another library's numbers.
- **Built entirely from the distribution's own packages**, the primary goal
  rather than a preference: link-time dependencies are libc and liburing, and
  nothing is bundled.
- **Correctness first, reasonable performance second.**
- **Scale with nodes, not with PEs.** Every algorithm has two levels: the PEs
  of a node meet in shared memory, and then the *nodes* meet. Nothing may
  scale with the number of PEs where it can scale with the number of nodes.
  Between nodes, algorithms are logarithmic in the node count, in time and in
  memory; within a node, linear or better in the PEs there.
- **Cores are not assumed spare.** A node runs a PE on every core with the
  library's progress thread beside them, so oversubscription is the normal
  case: nothing spins, and a waiting PE sleeps on a futex.
- **Launched by Slurm's `srun`, with no PMI library**: nodes find each other
  over TCP from what the step's environment says.
- **Loud, never hung.** A PE or a node that dies ends the whole job with a
  message naming it. No wait gives up merely because time has passed; a job
  proved deadlocked ends, naming where each PE waits, and a job told to
  (`OSOE_NO_PROGRESS_TIMEOUT`) ends a wait on the library's own progress that
  has stopped moving, and says what it waited for.
- **Observable from outside**: `osoe-stat` reads a running or hung job's
  counters and state from its control segment, without its cooperation.
- **A path to RDMA that no interface blocks.** The transport and collective
  seams are designed so that an RDMA (RoCE) backend could replace TCP at
  hundreds of nodes and toward 10⁶ PEs - a constraint on the design, not a
  backend this project builds (below).

## How osoe works

- **PEs are processes.** Each PE's symmetric heap is a shared-memory object,
  and every process on a node maps every heap on that node, its own at a fixed
  address the same in every PE. A put, get or atomic to a PE on the same node
  is a `memcpy` or a CPU atomic in the caller's own thread - no system call, no
  message - and `shmem_ptr` hands out a plain pointer to a node-mate's object.
- **One TCP connection per pair of nodes**, not per pair of PEs, carries every
  PE's operations between those two nodes as active messages. Its one ordered
  stream is what makes `shmem_fence` free: operations to a node are applied in
  the order they were sent.
- **One progress thread a node drives the network.** Every PE starts an agent
  thread; the node lead's is the only thread that touches a socket or an
  io_uring ring. It sends, receives, and applies arriving puts and atomics
  directly in the target PE's heap - that PE's own threads are never
  interrupted. The other agents do only what must happen inside their own
  process.
- **Waiting is sleeping.** A PE waiting for a word, a reply or buffer space
  sleeps on a futex, woken by whoever changed what it waits for.
- **Nodes find each other without PMI.** Each node's lead derives a port from
  Slurm's job, step and node ids, and the leads meet in a tree of fan-out 8
  rooted at the first node's; the PEs of a node meet in shared memory.
- **Everything above one boundary is backend-neutral.** The OpenSHMEM layer
  reaches other PEs only through `shmem/src/transport.h` - put, get, atomic,
  fence, wait, notify, all addressed as (PE, region, offset), never by
  address - and a collective's internode phase only through
  `shmem/src/internode.h`, addressed by (team, epoch). A test fails the build
  if any file above the boundary includes one below it. The one backend today
  is TCP and io_uring with an in-node shared-memory path.

## Scaling with nodes, not PEs

| Operation | How | Cost, for n nodes of P PEs |
|---|---|---|
| put, get, atomic to a node-mate | memcpy or CPU atomic on the shared heap | no system call |
| any of those across nodes | an active message on the node pair's one connection, applied by the target node's progress thread | connection state O(n) a node, not O(n × P) |
| barrier, sync, small reductions, broadcast | the node's PEs meet in shared memory; the node leads meet in a k-ary tree (`OSOE_COLLECTIVE_FANOUT`, default 4) | ⌈log_k n⌉ steps between nodes |
| larger data collectives | pieces pipelined through that tree, two in flight (`OSOE_COLLECTIVE_PIPELINE`) | |
| `fcollect` | Bruck's allgather among the node leads | ⌈log₂ n⌉ steps, each node's data crossing the network once |
| prefix scans | a tree over the team's ranks, its pieces pipelined as the other data collectives' are | O(log N) rounds |
| `alltoall`, `alltoalls` | every block put directly, then a barrier | O(N) a PE - inherent: every PE has a block for every other |
| a team | a number from a job-wide counter, and a record on each member node | O(1) a member after a one-time O(S log S) to build a split's node set |
| waiting | futex words per kind of resource, not per PE | O(1) a waiter |
| start-up | the node leads' rendezvous tree | ⌈log₈ n⌉ levels |
| `SHMEM_THREAD_MULTIPLE` | threads issue on a shared context without a lock; only quiet takes one, and concurrent quiets share one wait | O(1) an operation |

Where the design admits a limit of its own: large reductions still send each
node's whole vector up the tree and the result down, about twice the bytes of
a ring or Rabenseifner's algorithm, until measurement picks the crossover;
the tree's fan-out is one fixed default, where on the Pi cluster it proved a
bandwidth choice at large sizes; every pair of nodes connects at start-up, so
the job holds n(n - 1)/2 connections, n - 1 a node; and team numbers, never
reused, make a two-dimensional split of a whole job cost 2√N of them (below).

## Limits, and where osoe departs from the specification

Everything the specification requires of a C implementation is here. What
follows is every limit a program can meet, marked where it departs from the
specification or takes a choice the specification leaves open.

**Teams**

- **Team numbers are never reused.** A job can create `OSOE_MAX_TEAMS` - 3
  teams in its whole life - 61 by default, 1,021 at most - counting those
  already destroyed. Past that, a split returns nonzero on every PE of its
  parent team, as the specification allows for a split that cannot be made.
- **`shmem_team_split_2d` spends a number on every row and every column**:
  2√N for a square grid of N PEs, so no grid of a whole job above about 930
  PEs by default, or 2.6 × 10⁵ at the ceiling.

**Threads and contexts**

- **`shmem_init` provides `SHMEM_THREAD_SERIALIZED`** (a choice the
  specification leaves to the implementation); ask `shmem_init_thread` for
  `SHMEM_THREAD_MULTIPLE` to get it.
- **At most `OSOE_MAX_THREADS` live threads of a PE may have called the
  library**, the main thread included - 16 by default, 1,023 at most. A thread
  takes its place at its first call after `shmem_init` that does more than
  report on the job (`shmem_my_pe` takes none) and gives it back only when it
  exits, so workers that each make one call and live on count in full, though
  never two are in the library at once. A call that finds every place held by
  another live thread **ends the job**, naming the variable.
- **At most `OSOE_MAX_CONTEXTS` contexts a PE at once**, the default context
  included - 64 by default, 4,096 at most; past it `shmem_ctx_create` returns
  nonzero.
- **Communication sessions are accepted and do nothing** - conforming: they
  may not change what a program computes.

**Memory**

- **One symmetric heap a PE, its size fixed at start-up** by
  `SHMEM_SYMMETRIC_SIZE` (256 MiB by default), the same on every PE -
  checked, and a job that disagrees ends by name. At most 4 TiB a PE.
- **A node's heaps live in `/dev/shm`**, and start-up refuses a job whose
  heaps would not fit its free space (`OSOE_SHM_CAPACITY_CHECK=0` warns
  instead).
- **Global and static variables are symmetric only in a program linked by
  `oshcc`**, which links it without position independence and with osoe's
  linker script. Passing a static where a symmetric address is required in
  any other program ends the job by name.
- **Nonblocking gets and fetching atomics land in a per-PE landing zone**
  (`OSOE_GET_LANDING_BYTES`, 256 KiB by default): past it, a new one waits for
  earlier ones to land - backpressure, never an error.

**Launching**

- **Linux 6.7 or newer, and the distribution's liburing 2.6 or newer.** Other
  systems are not supported; on a Mac, develop in the Lima VM this repository
  defines.
- **Started by `srun` or by osoe's own launcher**, with no PMI library; every
  node must reach every other over TCP, the whole mesh, not only the
  rendezvous's port range; and the program must be at the same path on every
  node.
- **A host name that resolves only to IPv6 is refused.**
- **Run with `--cpu-bind=none`, or two CPUs a task.** Slurm binds each task to
  one core by default where a node has as many tasks as cores, and the node's
  progress thread then waits behind its first PE's own thread on that PE's
  core: on the Pi cluster that costs a pipelined point-to-point pattern six
  times its rate.
- **A login that ends on a node just before a job starts there can stall the
  job's start.** systemd-logind deletes a user's files in `/dev/shm` about ten
  seconds after the user's last login session on a machine ends (`RemoveIPC`,
  on by default), and a PE still in `shmem_init` may need a name there that its
  node-mates made. A running job is safe - osoe removes every name once init
  is done - but a start caught in those seconds can wait for ever. Set
  `RemoveIPC=no` in `logind.conf` on compute nodes, as PostgreSQL's
  documentation also advises, or enable lingering (`loginctl enable-linger`)
  for the users who run jobs.

**Failures**

- **A PE or a node that dies ends the whole job**, with a message naming it.
  There is no fault tolerance.
- **No wait gives up by default**: set `OSOE_NO_PROGRESS_TIMEOUT` for a wait
  on the library's own progress - acknowledgements, buffer space, a reply, the
  rendezvous - that stops moving to end the job naming what it waited for. A
  node that vanishes without closing its connections is noticed in one to two
  minutes by default (`OSOE_KEEPALIVE`): by TCP keepalive when nothing was in
  flight toward it, and by its kernel's silence when something was.
- **A program's own deadlock ends the job only once it is proved.** When
  every PE of the job waits inside the library, nothing moves on any node for
  `OSOE_DEADLOCK_SECONDS` (10 by default) and nothing is in flight between
  them, the job ends with status 1, and each node lists where its PEs wait - a
  collective one PE never entered, a wait for a word nobody writes. A PE
  computing for ever, or with any thread outside the library's waits - a
  helper thread, an idle OpenMP pool - is never judged deadlocked, since it may
  yet move: such a hang waits for ever, and `osoe-stat` shows where each PE is
  blocked. A program woken only from outside the library - by a signal
  handler's store, or another program writing memory it shares with a PE -
  should turn detection off: `OSOE_DEADLOCK_SECONDS=0`.

**Transport and API**

- **TCP only**; no RDMA backend exists (below).
- **`shmem_ptr` returns a pointer only for PEs on the same node**, NULL for the
  rest - which the specification allows.
- **Strided transfers move element by element, and block-strided ones block by
  block**, batched onto the connection: one message for the whole only when
  the blocks lie end to end.
- **C only.** There are no Fortran bindings. A C++ program can include
  `shmem.h`, and calls the typed routines: the type-generic interfaces are
  C11's `_Generic`, as the specification defines them.
- **`shmemx.h` is empty**, and the deprecated cache routines do nothing, as
  their deprecation allows.

## Toward RDMA: a RoCE backend

osoe has no RDMA backend, and building one is not this project's work before
its release point: no RoCE hardware is at hand. What the design guarantees is
that one could be added without touching the OpenSHMEM layer.

- **The seam is two headers.** `transport.h` is point-to-point: put, get, an
  atomic read-modify-write, fence, wait for completion, and notify a waiter,
  every location addressed as (PE, region, offset) - never a virtual address,
  since each process maps its node-mates' heaps wherever it can.
  `internode.h` is a collective's internode phase, addressed by (team,
  epoch), where a backend may run its own tree or a switch's. A backend is
  chosen at build time (`OSOE_BACKEND`, today only `am`), with no table of
  function pointers on the fast path.
- **What a RoCE backend would do**, as the design review of the seam sketched
  it (`docs/reviews/archive/roce-transport.md`): puts as RDMA writes, in three
  size classes, and gets as RDMA reads; atomics as the NIC's, which then serve
  node-mates too, since a NIC's atomics are not coherent with the CPU's; one
  registered memory region a node rather than a PE, its node's heaps laid out
  contiguously, so the key table stays 16 bytes a node; XRC queue pairs, one
  per context and target node, matching today's one connection a node pair;
  quiet as reaping completions, with a zero-length read where a write's
  completion does not promise visibility; and a per-node polling thread in
  place of the target-side counters RDMA lacks, to wake a PE waiting on a word.
- **What it would give up**: collectives stay above the seam, so in-network
  reduction (SHARP and its kind) is out of reach; `internode.h` is where a
  backend that can do better does it.
- **What is not known**: the review's hardware facts are recalled, not
  measured - eight questions stay open, the first of them load-bearing (whether
  a remote initiator can target a lead-registered region through a shared
  XRC domain). The one road to testing a verbs backend without hardware is
  Soft-RoCE (`rdma_rxe`), for function, not performance: post-release work,
  if hardware is still far off then.

## How osoe is tested

- **Four build presets** - `debug`, `release`, AddressSanitizer with
  UndefinedBehaviorSanitizer, and ThreadSanitizer - each with the whole suite
  run before every merge, on the exact tree that merges: about 620 tests in
  release; fewer under ThreadSanitizer, which cannot see an ordering that runs
  through another process, and so leaves most tests across nodes to the
  other three.
- **Model checking.** Every lock-free and futex protocol but one - waking the
  progress thread, waits, the barrier tree, pipelined collectives, Bruck's
  allgather, the scans, get completion under `SHMEM_THREAD_MULTIPLE`,
  rendezvous, abort, and the proof a deadlock is found by - is checked
  exhaustively in small configurations by an explicit-state model checker
  written for them (`tools/modelcheck`). Each
  model also carries deliberately broken variants that must fail: a model
  that cannot fail proves nothing. The one without a model is the deprecated
  active-set collectives' (`active-set.c`). The checker runs a model under
  sequential consistency or x86-TSO, and both keep a thread's loads in order,
  which aarch64, the machines osoe is built for, does not: the "blocked in"
  record's model passed while its routine name, then plain characters, could
  be read torn on aarch64. That name was the one plain field the round-three
  review's audit found rewritten while another process reads it; it is now
  four atomic words, and `shmem.blocked-record` runs the record's own writer
  against both of its readers on the machine.
- **Sandia OpenSHMEM's test suite**: 159 of its programs pass on two maps of
  logical nodes. The four that do not are SOS's own: two use an extension of
  the type-generic interfaces, one overflows its own buffer (osoe stops it by
  name), and one asks `shmem_align` for an alignment the specification leaves
  undefined.
- **Applications**: ISx, NPB IS, the Parallel Research Kernels and the
  OpenSHMEM examples, built with `oshcc`.
- **A stress tester**, `osoe-pacer`: whole-job patterns from real OpenSHMEM
  programs - a streaming sort, all-to-all exchanges, mailboxes, teams, threads,
  every atomic, transfer and collective, and all of them at once on teams out
  of step - every result checked against an oracle, and every oracle checked
  by a planted defect it must catch. A build of it that counts every call
  (`osoe-pacer-counted`, built where Python 3 is found) shows which of the
  library's 1,794 routines a run reaches.
- **A Raspberry Pi 5 cluster** over gigabit Ethernet, where puts, gets and
  atomics run within 8 to 13% of a bare io_uring TCP echo's round trip, streams
  run at line rate, and a 16-PE barrier takes about 180 µs
  (`docs/pi-cluster-results.md`).

## Reference

### Environment variables

Unless a row says otherwise, give every PE the same value: `srun` and
`osoe-launch` pass the environment to every task alike. The ones marked
*checked* are compared at start-up, and a job whose PEs disagree ends naming
the variable. `SHMEM_INFO` prints most of this list from the library itself.

**The specification's** (§8). Each also has a deprecated `SMA_` spelling; when
both are set the `SHMEM_` one controls.

| Variable | Default | What it does |
|---|---|---|
| `SHMEM_SYMMETRIC_SIZE` | `256m` | Symmetric heap bytes a PE, with an optional `k`, `m`, `g` or `t` multiplier; at most 4 TiB. *Checked*. |
| `SHMEM_VERSION` | unset | Set: every PE prints the library and the specification's version at start-up. |
| `SHMEM_INFO` | unset | Set: each node's first PE prints the variables osoe reads, with their defaults. |
| `SHMEM_DEBUG` | unset | Set: every PE prints what the library set up for it - its rank and node, its heap's size and address, its control segment's name. |

**Limits**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_MAX_TEAMS` | `64` | Team numbers a job uses in its life, the three predefined teams' included: `4` to `1024`. A destroyed team keeps its number, so this bounds the teams a job creates - 61 at the default - not only those alive at once. Each number costs every PE about 160 KiB of address space that only a team's leads touch. *Checked*. |
| `OSOE_MAX_CONTEXTS` | `64` | Contexts a PE may have at once, the default context included: `1` to `4096`. *Checked* among a node's PEs. |
| `OSOE_MAX_THREADS` | `16` | Live threads of a PE that may have called the library, the main thread included: `1` to `1023`. A thread's place is taken at its first call after `shmem_init`, not counting queries such as `shmem_my_pe`, and freed when it exits, not when the call returns; a call that finds none free ends the job, naming the variable. The same on every PE of a node. |
| `OSOE_GET_LANDING_BYTES` | `262144` | Bytes of each PE's landing zone for the replies of its gets and fetching atomics, a multiple of `1024` up to `67107840`: past it, a new get waits for earlier ones to land. |

**Collectives**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_COLLECTIVE_FANOUT` | `4` | k of the tree the node leads meet in: `2` to `32`. Over Ethernet it is a bandwidth choice at large sizes, since a node sends a copy to each of its children over its one link. *Checked*. |
| `OSOE_COLLECTIVE_PIPELINE` | `2` | Pieces of a data collective each node keeps in flight up and down that tree: `1` to `8`. *Checked*. |

**Start-up, failures and inspection**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_NO_PROGRESS_TIMEOUT` | `0` | Seconds a wait on the library's own progress - acknowledgements, buffer space, a reply, the rendezvous - may see none before it ends the job, naming what it waited for; `0`, never. A program's own waits and collectives are not timed. |
| `OSOE_KEEPALIVE` | `60,10,6` | `IDLE,INTERVAL,COUNT`: TCP keepalive between nodes, in seconds, seconds and probes. Their total is also how long a node's kernel may leave data sent to it unacknowledged before the node counts as vanished. `0` for neither. |
| `OSOE_DEADLOCK_SECONDS` | `10` | How long a job whose every PE waits in the library, with nothing moving on any node and nothing in flight, is watched before it ends as deadlocked, each node naming where its PEs wait: `1` to `86400`, or `0` for never. |
| `OSOE_PORT_RANGE` | `17000-26999` | `LOW-HIGH`, within `1024-65535`: the ports a node's lead may listen on for the rendezvous. |
| `OSOE_JOB_SECRET` | unset | 32 hexadecimal digits: a secret every node proves it knows when it joins the job, never sent. |
| `OSOE_HEAP_BASE` | chosen | The address every PE maps its heap at, a multiple of the page size, if the library's own candidates are taken. The node's first PE's value is the node's. |
| `OSOE_SHM_CAPACITY_CHECK` | `1` | `0`: a node whose heaps would not fit `/dev/shm`'s free space is warned, not refused. |
| `OSOE_KEEP_CONTROL_SEGMENT` | `0` | `1`: the control segment keeps its name in `/dev/shm` until finalize, so a crash leaves it for `osoe-stat` to read; `osoe-stat --gc` removes such segments once their PEs are gone. |

**The progress thread**: each PE's agent thread, which on a node's first PE
moves every message of the node

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AGENT_SLICE_US` | `100` | The scheduler slice each agent thread asks for, in microseconds: `100` to `100000`, or `0` for the kernel's own. A short slice lets it preempt sooner when it wakes on a core it shares - the same fair share, no priority over anyone. Linux 6.12 and later; an older kernel ignores it. |
| `OSOE_AGENT_CPUS` | `task` | Where each agent thread may run: `task`, on its own PE's CPUs; `node`, on any CPU any PE of its node was bound to - the job's own cores there, never another's. The PEs' threads keep their binding either way. `node` lifts a bound job's messages where the kernel ignores the slice (a fifth on the Pi's mailbox), but beside the slice it made the Pi's small collectives 7-18% slower. The thread is named `osoe-agent`, as `top -H` and `ps -L` show it. |

**The transport's buffers**, which a program rarely needs to change

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AM_CHUNK` | `65536` | The largest message on a connection: a power of two from `1024` to `1048576`; larger transfers are split. |
| `OSOE_AM_BUFFER` | twice the chunk | Bytes of each connection's buffer: a power of two up to 1 GiB. |
| `OSOE_AM_BUFFER_BUDGET` | 256 MiB | A node's ceiling on its connections' buffers together; the buffers shrink, saying so, to fit. |
| `OSOE_AM_CREDITS` | `64` | A connection's flow-control credits: `1` to `65536`. |
| `OSOE_AM_HUGEPAGES` | `0` | `1`: map the node's connection buffers with huge pages, falling back, with a warning, when there are none. |
| `OSOE_WIRE_BYTES`, `OSOE_WIRE_BUFFERS` | `16384`, `64` | The size and number of the buffers io_uring receives into. |

**For testing**

| Variable | What it does |
|---|---|
| `OSOE_HEAP_SHIFT` | A size: each PE maps its heap that far times its rank past the base, so no two PEs share a symmetric address - how a program is checked for assuming they do. |
| `OSOE_FORCE_REMOTE` | `1`: node-mates talk over the network instead of shared memory, and `shmem_ptr` answers NULL for them; needs two logical nodes or more. |
| `OSOE_CHECK_QUIET` | `1`: every quiet checks that it completed what it promises, and ends the job naming what it did not. |
| `OSOE_TEST_PERTURB` | `1`: short sleeps and yields at the library's internal choice points, to widen races. |
| `OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED` | A rate per 10,000 at which sends and receives are cut short, and the seed that makes a failing pattern repeat. |

**The tools'**: `OSOE_CC` names the C compiler `oshcc` runs; `OSOE_NODE_MAP`
is `osoe-launch`'s `-m`. `OSOE_BOOTSTRAP_TIMEOUT`, `OSOE_WATCHDOG` and
`OSOE_LIVENESS` are retired, and ignored with a notice.

### Starting PEs without a launcher

`srun` and `osoe-launch` set nine bootstrap variables in each PE; the library
reads them as `SLURM_` or `OSOE_` names, whichever set is there (`OSOE_PROCID`
decides). To start one process per PE yourself, set these in each; a program
started with none of them fails at `shmem_init` with "no launcher
environment". There is no single-PE default.

| Variable | Value for a one-node job of N PEs |
|---|---|
| `OSOE_PROCID` | This PE's rank, `0` … `N-1` |
| `OSOE_STEP_NUM_TASKS` | `N` |
| `OSOE_NODEID` | `0` |
| `OSOE_STEP_NUM_NODES` | `1` |
| `OSOE_LOCALID` | This PE's position in `OSOE_GTIDS`; `0` is the node's lead |
| `OSOE_GTIDS` | The ranks on this node, comma-separated: `0,1,…,N-1` |
| `OSOE_JOB_ID` | A number unique among jobs running on this machine at once |
| `OSOE_STEP_ID` | A number; `0` is fine |
| `OSOE_STEP_NODELIST` | The nodes' host names, as Slurm writes a host list; needed only for more than one node |

- The job id and step id name the job's shared memory
  (`/dev/shm/osoe.<job>.<step>.…`). Two jobs running at once with the same pair
  collide, and the second fails at init.
- `OSOE_GTIDS[OSOE_LOCALID]` must equal `OSOE_PROCID`.

For example, two PEs from a shell:

```sh
export OSOE_STEP_NUM_TASKS=2 OSOE_STEP_NUM_NODES=1 OSOE_NODEID=0 OSOE_GTIDS=0,1
export OSOE_JOB_ID=$$ OSOE_STEP_ID=0
OSOE_PROCID=0 OSOE_LOCALID=0 ./my_program &
OSOE_PROCID=1 OSOE_LOCALID=1 ./my_program
wait
```

A job that crashes can leave `/dev/shm/osoe.<job>.*` behind. Remove it before
reusing the same job id.

### How a job ends

A PE that dies, or a node that vanishes, ends the whole job with a failure
(design §3.15). `shmem_global_exit(status)` ends every PE with `status`: the
calling PE with `exit`, so its `atexit` handlers run and its streams are
flushed, as the specification asks; every other PE with its standard I/O
flushed and then `_exit`, so its handlers do not run - it is usually still
computing - and output it holds in a buffer of its own, outside stdio, is lost.
A job proved deadlocked ends the same way as a failure, with status 1, after
each node has printed where its PEs wait.

### Further reading

- [Design notes](docs/design/shmem-ethernet-design.md) - every protocol and data
  structure, and why
- [The Pi cluster's results](docs/pi-cluster-results.md), and its
  [runbook](docs/pi-cluster.md)
- [Decisions in force](docs/decisions/README.md), each linked to the dated
  record that explains it
- [Specification reference](docs/reference/README.md)

## License

osoe is free software: you can redistribute it and modify it under the terms
of the GNU Lesser General Public License, version 3 or (at your option) any
later version ([COPYING.LESSER](COPYING.LESSER)), which supplements the GNU
General Public License, version 3 ([COPYING](COPYING)). Every source file says
so in one line, `SPDX-License-Identifier: LGPL-3.0-or-later`. A program linked
against the library may carry any license its authors choose; changes to the
library itself are shared under the same terms.
