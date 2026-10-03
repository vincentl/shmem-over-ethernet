# osoe

**OpenSHMEM over Ethernet**: an implementation of the OpenSHMEM 1.6
specification for clusters connected by ordinary Ethernet. It needs no RDMA
hardware and no fabric library. It builds from a Linux distribution's own
packages - a C compiler, CMake, pkg-config and liburing - and carries every
operation between nodes over TCP, driven by io_uring. It was developed in an
aarch64 virtual machine and on a cluster of four Raspberry Pi 5 boards, and is
designed to stay usable at hundreds of nodes with 256 processes each - a scale
it has not run at: the largest job across separate machines so far is the Pi
cluster's 16 PEs, four on each node.

**Status.** Every routine of OpenSHMEM 1.6's C interface is implemented, the
deprecated ones of its Annex F included. What departs from the
specification, or takes a choice it leaves open, is listed under
[Limits](#limits-and-where-osoe-departs-from-the-specification). osoe has not
made a release yet: there is no version tag, and it is built and tested from
the repository as it stands. The tree is version 0.1.0; [Installing](#installing)
says where the library and the tools show it.

## OpenSHMEM in two minutes

OpenSHMEM is a library for SPMD parallel programs, like MPI: a launcher starts
N copies of one program, the *processing elements* (PEs), numbered 0 to N - 1.
What differs is how they communicate.

- **One-sided.** Every PE allocates the same objects, in the same order, from
  its *symmetric heap* (`shmem_malloc` is collective), so the address of its own
  copy names the matching object on every PE; global and static variables are
  symmetric in the same way. `shmem_put(dest, source, n, pe)` writes into PE
  `pe`'s copy of `dest`, and `shmem_get` reads from it: the target takes no
  part, and is not interrupted.
- **Atomics** - fetch-and-add, compare-and-swap, swap, and, or, xor - update a
  word on any PE.
- **Completion and ordering are explicit.** A put returns once its source may be
  reused, but the data may still be in flight: `shmem_quiet` waits until every
  put this PE issued has landed, and `shmem_fence` keeps puts to one PE in
  order. A barrier does both and synchronizes.
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
srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 ./ring
```

## Getting started

### Requirements

- Linux 6.7 or newer, and for a job of more than one node io_uring open to its
  user: the `kernel.io_uring_disabled` sysctl at 0, or at 1 with the user in the
  group `kernel.io_uring_group` names (`env.probe`, below, checks)
- liburing 2.6 or newer
- CMake 3.25 or newer, pkg-config, and GCC or Clang: the code is C11 with GNU
  extensions, the symbol attributes and `__typeof__`
- Python 3, optionally: the model checker and the stress tester's counting
  build use it. Where CMake does not find it, it says so while configuring and
  leaves those out; nothing else needs it.

osoe has run on aarch64 only, on the Raspberry Pi cluster and in an aarch64
virtual machine. An x86-64 build of an earlier tree passed most of the one-node
tests an emulator on an aarch64 kernel can run, with a shim standing in for the
futex2 calls the emulator refuses, and no x86-64 kernel has run it: see Limits,
"Launching".

Debian 13 (Trixie) and Raspberry Pi OS Trixie provide all of these from their
own repositories - `sudo apt install build-essential cmake pkg-config
liburing-dev`, and `python3` for the models - and a node that only runs jobs
needs only liburing's runtime package, `liburing2`. Older releases are not
supported, even where the kernel is new enough, since osoe does not bundle a
newer liburing (design notes, §1): Debian 12 has liburing 2.3 and Ubuntu 24.04
2.5, and Rocky Linux 9 and its kin have Linux 5.14.

### Building

Builds use CMake presets and run on Linux only. Each preset builds under
`build/<preset>`, which git ignores:

```sh
cmake --workflow --preset release    # configure, build, and test, without the models
```

That builds one file at a time and runs the tests one at a time - more than 650,
checks of the build itself and jobs of a few processes. On four cores of an idle
virtual machine it took about eight and a half minutes, one for the build and
seven and a half for the tests. Both may run four at a time, as this project's
own runs do (`cmake --build --preset release -j4`, `ctest --preset release
-j4`). The 24 tests of the model checker (`tools/modelcheck`, which needs Python
3) took 29 minutes in the development VM, one at a time, and check the design,
which does not vary with the machine. They have a label of their own, `model`,
which the `release` preset leaves out and the `models` preset runs alone, in the
same build:

```sh
ctest --preset models                # the models, once the release build exists
cmake --workflow --preset models     # or from a clean tree: configure, build, then them
```

Presets: `debug`, `release` (optimized, with debug info), `asan`
(AddressSanitizer and UndefinedBehaviorSanitizer), and `tsan` (ThreadSanitizer);
and `tier3`, the release build with tests over network namespaces whose links
`netem` shapes, which needs `sudo` without a password, and `ip`, `tc` and
`ethtool`.

The `env.probe` test checks that the running kernel provides every io_uring and
futex facility osoe relies on, `shmem_init`'s own checks among them. A job meets
the compute nodes' kernels, so run it there too, from a build tree every node
sees - it is not installed - as the Pi cluster's runbook does: `srun -N4
--ntasks-per-node=1 build/release/tools/probe/probe`. If a futex check fails, no
job will run there: osoe needs the futex2 calls of Linux 6.7 for a job of any
size. If only io_uring checks fail - the kernel's `kernel.io_uring_disabled`
sysctl is set, say - a job on one node still runs, since it opens no ring; a job
of more than one node is refused at `shmem_init`, naming the facility that is
missing, for the io_uring facilities `shmem_init` checks (`env.probe` tests a
few more).

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
`mpp/` forms older programs include - `oshcc` and the linker script it passes,
`osoe-stat` and `osoe-pacer`: all that a node running jobs needs, and a check
that osoe works there. The installed programs find the rest from where they are,
so the prefix may be chosen at install time. A program built by `oshcc` loads
the library from the prefix's absolute path, so install under a path that every
node sees the same - a shared file system, or the same install on each - and
give every node liburing's runtime package. `osoe-launch` and `osoe-bench` stay
in the build tree: they are for development, tests and measurement. Then put the
prefix's `bin` on your `PATH`.

Once installed, run the stress tester's smoke test under the cluster's launcher:

```sh
srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 /opt/osoe/bin/osoe-pacer --quick
```

It runs its exercises at small sizes, checks every result, and ends with a line
beginning `osoe-pacer failures=0` and exit status 0 - in 7 s on the Pi cluster's
16 PEs. `osoe-pacer --help` lists what else it runs.

The version is set once, by `project(osoe VERSION ...)` in `CMakeLists.txt`,
and everything that names a version says it: `SHMEM_VERSION=1` makes every PE
print a line ending `osoe 0.1.0, OpenSHMEM 1.6` at start-up, and `SHMEM_INFO=1`
heads its text with `osoe 0.1.0 environment variables`; `osoe-stat`,
`osoe-pacer`, `osoe-launch` and `osoe-bench` answer `--version` with one line,
such as `osoe-stat (osoe) 0.1.0`; the shared library is `libosoe.so.0.1.0`, with
the soname `libosoe.so.0`. `oshcc` answers `--showme:version` instead: its
`--version` is still the C compiler's, which build systems run to learn what
`CC` is. (`shmem_info_get_name` still returns `osoe`, a name, and
`shmem_info_get_version` the specification's, 1.6.)

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
names another compiler, a C++ one included: `OSOE_CC=g++ oshcc` builds a C++
program with its statics symmetric, as the Pi cluster's runs of the OpenSHMEM
test suite's C++ programs did.

### Running a program with `srun`

```sh
srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 ./my_program
```

No PMI plugin is involved: each node's *lead* - its first PE, whose progress
thread does all of the node's networking once the job has started - finds the
others over TCP, on a port it derives from the job, step and node ids
(`OSOE_PORT_RANGE`, default 17000-26999), and then connects to each on a port
the kernel chooses, so TCP must be open between the nodes, not only that range.
Every node must resolve the names in the step's node list, each name to an
address at which the other nodes reach that node: a lead listens at the address
its own host resolves its name to - or, where that is a loopback address, as
Debian's `/etc/hosts` makes it, at the address its host would send from toward
another node - and the others dial the address they resolve it to. So osoe uses
whichever network those names resolve to; there is no setting for an interface,
and a cluster that wants its data network used maps the node names to that
network's addresses. `srun` runs the same path on every node, so the program
must be at that path on each.

- **`-K1`** (or `KillOnBadExit=1`), so that a task that fails at start-up ends
  the rest rather than leaving them waiting (design §3.15): the library ends a
  node whose PE fails, and the nodes already connected to it, but not a node
  that has not reached it yet, nor anything waiting for a task that never
  reached the library ("How a job starts", below). A job ended by
  `shmem_global_exit(status)` still reaches `srun` as `status` under `-K1`:
  every PE but the caller leaves with 0, and the caller leaves last, so its exit
  is the one bad one Slurm sees - with the limits under "How a job ends".
- **`--cpu-bind=none`**; `SLURM_CPU_BIND=none` in the environment does the same.
  Where a node runs as many tasks as it has cores, Slurm binds each task to one
  core by default, and the thread that moves every message of the node - the
  progress thread of its first PE - is bound with that PE: it waits behind the
  PE's own thread while the other cores idle. On the Pi cluster that cost a
  pipelined point-to-point pattern six times its rate; the short scheduler slice
  osoe asks for that thread (`OSOE_AGENT_SLICE_US`, below) makes it four, and
  only unbinding the tasks recovers the rest. Unbinding costs as well. On the Pi
  cluster most small collectives took 4 to 17% longer unbound - a 16-PE barrier
  196 to 208 µs against 176 to 183, an 8-byte broadcast 4% longer, an 8-byte
  reduce 6%, a 4 KiB fcollect 16% and a 64 KiB reduce 17%, where alltoall and a
  64 KiB broadcast stayed within 1% - and a stream of 64 KiB puts between PEs of
  one node fell from 23,300 MiB/s to 13,600. So a program bound by small
  collectives or by traffic within a node, rather than by pipelined messages
  between nodes, may prefer Slurm's binding.
- **`OSOE_NO_PROGRESS_TIMEOUT=60`** while a cluster is new, so that a wait on
  the library's own progress - a connection's acknowledgements, buffer space,
  a reply - that stops moving ends the job and says what it waited for.
- **`OSOE_JOB_SECRET`**, 32 hexadecimal digits exported before `srun`, keeps a
  host that does not know it from joining the job (design §6.2) - and no more:
  see "Trust", under "How a job starts". Without it, any host that can reach the
  ports can join, so on a cluster shared with other users give every job one.

A batch script that does all of this:

```sh
#!/bin/sh
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=4
export OSOE_JOB_SECRET=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
export OSOE_NO_PROGRESS_TIMEOUT=60      # while the cluster is new
srun -K1 --cpu-bind=none ./my_program
```

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
expires, or if the job leaves shared memory behind, and its exit status is
`srun`'s: the highest exit code of any rank - so a job ended by
`shmem_global_exit(7)` exits 7 - or 128 plus the signal for a rank killed by
a signal the launcher did not send itself, and 1 for a timeout or shared
memory left behind when no rank reported more (`--help` says the same).
`-e RANK:NAME=VALUE` sets a variable for one rank only. `-k` leaves the surviving processes alone when one
fails, instead of terminating them, so that what ends the job is the library's
own reaction rather than the launcher's.

### Looking inside a job

`osoe-stat` reads a running or hung job's counters and state from its node's
control segment - the shared-memory object in which a node's PEs meet - without
the job's cooperation. It reads the node it runs on, so run it on each of the
job's nodes, naming the job by its id - Slurm's job id, or for a job of
`osoe-launch` the launcher's process id:

```sh
osoe-stat 1234            # job 1234 on this node: counters, and where each PE waits
osoe-stat --summary 1234  # each team's collective in progress, to walk a hang down
```

A job that hangs shows, for every PE, the routine it is blocked in, the
collective's epoch - how many collectives its team has begun - and for how long.
A job that deadlocks - every PE waiting in the library, with nothing in flight
between its nodes - ends by itself after `OSOE_DEADLOCK_SECONDS` and prints the
same, node by node (below); a hang the library cannot prove, such as a PE
computing for ever, is what `osoe-stat` is for.

### When a job fails or hangs

- **The exit status says who ended it.** Every PE the library ends leaves with
  1 - a PE or a node that died, a start refused, a deadlock proved, a wait that
  `OSOE_NO_PROGRESS_TIMEOUT` ended. The PE that saw the cause writes a line on
  standard error saying why; each of the others, one naming that PE. A status
  given to `shmem_global_exit` comes through as it is. Under `srun -K1` a task
  Slurm killed, once another had ended badly, counts 143 (128 plus SIGTERM):
  look for the first message, not the highest status.
- **A start that waits** says what for after 30 s, one line a node ("How a job
  starts", below). A task that died before `shmem_init` is waited for until the
  launcher ends the step, or `OSOE_NO_PROGRESS_TIMEOUT`, if it is set, ends the
  wait.
- **A job that hangs**: `osoe-stat JOB` and `osoe-stat --summary JOB` on each of
  its nodes, above. A deadlock the library can prove ends the job by itself
  after `OSOE_DEADLOCK_SECONDS` (Limits, "Failures").
- **Names a failed start left in `/dev/shm`**, and what removes them: "Names a
  failed start leaves", below.
- **A start that waits for ever just after a login on a node ended** may be
  systemd-logind's `RemoveIPC` (Limits, "Launching").
- **Collectives that stall for seconds at some sizes**, while the kernel's count
  of TCP timeouts climbs (`nstat -az TcpExtTCPTimeouts`), are what the Pi
  cluster met as F1. Turning the segmentation offload off on the node that
  stalled - `ethtool -K <interface> tso off gso off` - ended the stalls it
  measured, though its cause is not settled ("How osoe is tested").

## Design goals

- **Commodity Ethernet, no fabric offload.** Every operation between nodes is
  carried by the host CPU over TCP. The design notes judge performance against
  a local floor - a bare io_uring TCP echo, a stream, a memcpy - rather than
  against another library's numbers.
- **Built entirely from the distribution's own packages**, the primary goal
  rather than a preference: link-time dependencies are libc and liburing, and
  nothing is bundled.
- **Correctness first, reasonable performance second.**
- **Scale with nodes, not with PEs.** The collectives have two levels: the PEs
  of a node meet in shared memory, and then the *nodes* meet. Nothing may scale
  with the number of PEs where it can scale with the number of nodes. Between
  nodes the collectives are logarithmic in the node count, in time and in
  memory - but for the prefix scans, a tree over the team's ranks; alltoall, in
  which every PE has a block for every other; collect's lengths, a word a PE;
  and the deprecated active-set collectives, which run PE by PE - while the
  connection mesh is linear in it, n - 1 connections a node ("Scaling with
  nodes", below); within a node, linear or better in the PEs there - tested with
  at most 15 PEs on a node, and timed with 4 on the Pi cluster and, for the
  barrier alone, up to 32 on one four-core virtual machine (design §5.3): a
  node's barrier arrival is one cache line that all its PEs add to, and in a
  reduction the node's lead PE combines the node's vectors one after another on
  its own thread, O(PEs × bytes).
- **Cores are not assumed spare.** A node runs a PE on every core with the
  library's progress thread beside them, so oversubscription is the normal case:
  nothing spins. A waiting PE sleeps on a futex, and a start-up wait for an
  object not yet made sleeps between looks.
- **Launched by Slurm's `srun`, with no PMI library**: nodes find each other
  over TCP from what the step's environment says.
- **Loud, never hung.** A PE or a node that dies ends the whole job with a
  message naming it. No wait gives up merely because time has passed; a job
  proved deadlocked ends, naming where each PE waits, and a job told to
  (`OSOE_NO_PROGRESS_TIMEOUT`) ends a wait on the library's own progress that
  has stopped moving, and says what it waited for. Loud stops where the library
  cannot see. The PEs of one node are not compared with each other on the
  collectives they call, so a collective they call differently goes unnamed
  (Limits, "Failures").
- **Observable from outside**: `osoe-stat` reads a running or hung job's
  counters and state from its control segment, without its cooperation.
- **A path to RDMA that no interface blocks.** The transport and collective
  seams are designed so that an RDMA (RoCE) backend could replace TCP at
  hundreds of nodes and toward 10⁶ PEs - a constraint on the design, not a
  backend this project builds (below).

## How osoe works

- **PEs are processes.** Each PE's symmetric heap is a shared-memory object, and
  every process on a node maps every heap on that node, its own at a fixed
  address the same in every PE. A put, get or atomic to a PE on the same node is
  a `memcpy` or a CPU atomic in the caller's own thread - no message, and no
  system call unless the target waits on what it changed, which takes one futex
  wake - and `shmem_ptr` hands out a plain pointer to a node-mate's object.
- **One TCP connection per pair of nodes**, not per pair of PEs, carries every
  PE's operations between those two nodes as active messages. Its one ordered
  stream is what lets `shmem_fence` return without waiting: operations to a node
  are applied in the order they were sent, so a fence only asks each node
  written to since the last acknowledgement to acknowledge, and goes on.
- **One progress thread a node drives the network.** Every PE starts an agent
  thread; once the job has started, the node lead's is the only thread that
  touches a socket or an io_uring ring. It sends, receives, and applies arriving
  puts and atomics directly in the target PE's heap - that PE's own threads are
  never interrupted. The other agents do only what must happen inside their own
  process.
- **Waiting is sleeping.** A PE waiting for a word, a reply or buffer space
  sleeps on a futex, woken by whoever changed what it waits for.
- **Nodes find each other without PMI.** Each node's lead derives a port from
  Slurm's job, step and node ids, and the leads meet in a tree of fan-out 8
  rooted at the first node's; the PEs of a node meet in shared memory.
- **Everything above one boundary is backend-neutral.** The OpenSHMEM layer's
  point-to-point operations reach other PEs only through
  `shmem/src/transport.h` - put, get, atomic, fence, wait, notify, all addressed
  as (PE, region, offset), never by address - and a collective's internode phase
  only through `shmem/src/internode.h`, addressed by (team, epoch); a
  collective's phase within a node reads and writes its node-mates' heaps
  directly. A test, `shmem.boundary`, fails if any file above the boundary
  includes one below it that it may not. The one backend today is TCP and
  io_uring with an in-node shared-memory path.

## Scaling with nodes, not PEs

| Operation | How | Cost, for n nodes of P PEs, N PEs in all, and a team of M members on S nodes |
|---|---|---|
| put, get, atomic to a node-mate | memcpy or CPU atomic on the shared heap | no system call, but a futex wake when the target waits on the word |
| any of those across nodes | an active message on the node pair's one connection, applied by the target node's progress thread | the connections' state O(n) a node, not O(n × P); each PE's ordering state O(n) a context, 21 bytes a node |
| barrier, sync, small reductions, broadcast | the node's PEs meet in shared memory; the node leads meet in a k-ary tree (`OSOE_COLLECTIVE_FANOUT`, default 4) | ⌈log_k n⌉ steps between nodes |
| larger data collectives | pieces pipelined through that tree, two in flight (`OSOE_COLLECTIVE_PIPELINE`) | O(P × bytes) for the lead's combine on a node, then the tree |
| `fcollect` | Bruck's allgather among the node leads | ⌈log₂ n⌉ steps, each node's data crossing the network once |
| `collect` | the PEs' lengths first, as one vector of 8 bytes a PE, summed through the node leads' tree, then the data as `fcollect`'s | the data as `fcollect`'s; the lengths O(N) bytes up the tree and down it, 1 MB at 128,000 PEs |
| prefix scans | a tree over the team's ranks, its pieces pipelined as the other data collectives' are | O(log N) rounds |
| `alltoall`, `alltoalls` | every block put directly, then a barrier | O(N) a PE - inherent: every PE has a block for every other |
| a team | a number from a job-wide counter, and a record on each member node | O(P) a member, testing its node's PEs; on each node's lead, O(M) once to find the team's nodes from its members - O(M log M) when they are not met in node order - and the S leads kept (temporary: the team work replaces this) |
| waiting | a futex word per resource a connection has, shared by the node's PEs; one per thread for its replies and watches, per PE for its agent and its landing zone, per context for its counters | O(1) a waiter |
| start-up | the node leads' rendezvous tree, then their mesh | ⌈log₈ n⌉ levels, then n - 1 connections a lead, made one after another |
| `SHMEM_THREAD_MULTIPLE` | threads issue on a shared context without a lock; every wait that completes its operations - a quiet, `shmem_pe_quiet`, a strided get's end, and the quiet a barrier or `shmem_clear_lock` makes - takes the context's, and one shares a full quiet that began and ended while it waited for it | O(1) an operation |

Where the design admits a limit of its own: large reductions still send each
node's whole vector up the tree and the result down, so the busiest lead sends
k + 1 vectors where a ring or Rabenseifner's algorithm sends about two - 2.5
times as many bytes at the default fan-out of 4 - until measurement picks the
crossover; the tree's fan-out is one fixed default, where on the Pi cluster it
proved a bandwidth choice at large sizes - on its four nodes a fan-out of 2 with
the pipeline 4 to 8 deep was 1.5 times faster at 1 MiB than the default, though
at the default depth of 2 it sped a broadcast 1.2 times and slowed a reduction
19%, and 4 was twice as fast up to 1 KiB; every pair of nodes connects at
start-up, so the job holds n(n - 1)/2 connections, n - 1 a node; one thread a
node moves all of its internode messages (Limits, below); and team numbers,
never reused, make a two-dimensional split of a whole job cost 2√N of them
(below; temporary).

## Limits, and where osoe departs from the specification

Everything the specification requires of a C implementation is here. What
follows is every limit a program can meet, marked where it departs from the
specification or takes a choice the specification leaves open.

**Teams**

The three limits below are temporary: the team work, decided and to be built
before the first release (`docs/design/team-resources-proposal.md`), gives each
PE numbers of its own, given back when a team is destroyed.

- **Team numbers are never reused.** A job can create `OSOE_MAX_TEAMS` - 3 teams
  between `shmem_init` and `shmem_finalize` - 61 by default, 1,021 at most -
  counting those already destroyed. Past that, a split returns nonzero on every
  PE of its parent team, as the specification allows for a split that cannot be
  made.
- **`shmem_team_split_2d` spends a number on every row and every column**:
  2√N for a square grid of N PEs, so no grid of a whole job above about 930
  PEs by default, or 2.6 × 10⁵ at the ceiling.
- **Every split claims job-wide numbers, so a split made on each node spends one
  a node.** A split takes its numbers from one job-wide counter whatever its
  parent team, so one line of a program - a split of `SHMEM_TEAM_SHARED` on
  every node, say, which is a separate split on each - spends a number for every
  node. With the default `OSOE_MAX_TEAMS` 61 nodes get their team; on a job of
  62 nodes or more the rest get `SHMEM_TEAM_INVALID` and a nonzero return -
  which nodes is a race - and a program that ignores the return runs that team's
  collectives as no-ops there and ends with status 0. Raising `OSOE_MAX_TEAMS`
  costs every PE about 200 KiB of address space a number, which the `/dev/shm`
  check counts.

**Threads and contexts**

- **`shmem_init` provides `SHMEM_THREAD_SERIALIZED`** (a choice the
  specification leaves to the implementation); ask `shmem_init_thread` for
  `SHMEM_THREAD_MULTIPLE` to get it.
- **`shmem_init_thread` returns 0 or does not return.** A start-up that fails
  ends the program, with a message and status 1, where §9.2.1 has the routine
  return nonzero - a choice: the specification allows ending the program for an
  invalid `SHMEM_SYMMETRIC_SIZE` (§8), and in a job of many processes a PE that
  returned would leave the rest waiting for it.
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
- **Any team can create contexts, whatever its `num_contexts`.** The
  specification's default is a team that can create none (§9.4.3), and asks only
  for a best effort for as many as a split names. osoe records the number a
  split was given - `shmem_team_get_config` returns it - but
  `shmem_team_create_ctx` never looks at it, and fails only when the PE's
  `OSOE_MAX_CONTEXTS` are all taken or memory is short. Permissive: no program
  that follows the specification can see it.
- **Communication sessions are accepted and do nothing** - conforming: they
  may not change what a program computes.

**Memory**

- **One symmetric heap a PE, its size fixed at start-up** by
  `SHMEM_SYMMETRIC_SIZE` (256 MiB by default), the same on every PE -
  checked, and a job that disagrees ends by name. At most 4 TiB a PE. A size of
  0, which the specification's grammar admits, ends the job at start-up, naming
  the variable - a choice: no `shmem_malloc` could succeed, and a zero is
  usually a script's arithmetic on an unset variable gone wrong.
- **A node's heaps live in `/dev/shm`**, and start-up refuses a job whose
  heaps would not fit its free space (`OSOE_SHM_CAPACITY_CHECK=0` warns
  instead).
- **At the defaults a job has at most 1,025 nodes.** A node's connection
  buffers - two for each other node, 128 KiB each - must fit
  `OSOE_AM_BUFFER_BUDGET`, 256 MiB: the buffers of 1,025 nodes come to exactly
  that and 1,026 are refused at start-up, naming the budget. The budget
  shrinks the buffers only down to twice `OSOE_AM_CHUNK`, and the default
  buffer is that size, so at the defaults nothing can shrink: raise the budget
  or lower the chunk. Where a raised `OSOE_AM_BUFFER` does shrink, each node's
  lead prints one line saying so.
- **Global and static variables are symmetric only in a program linked by
  `oshcc`**, which links it without position independence and with osoe's
  linker script. Passing a static where a symmetric address is required in
  any other program ends the job by name.
- **A PE's gets from other nodes land in its landing zone**
  (`OSOE_GET_LANDING_BYTES`, 256 KiB by default, in extents of 1 KiB): a get
  takes whole extents, at least one, so 256 small gets in flight fill it as 256
  KiB of data does, and past it a new get waits for earlier ones to land -
  backpressure, never an error.
- **A nonblocking fetching atomic completes before it returns.**
  `shmem_atomic_fetch_add_nbi` and its kin issue the same round trip as the
  blocking forms and leave the value in place at once - the earliest the
  specification allows - so between nodes a run of them costs a round trip each,
  rather than overlapping as nonblocking gets do.

**Launching**

- **Linux 6.7 or newer, and the distribution's liburing 2.6 or newer.** Other
  systems are not supported; on a Mac, develop in the Lima VM this repository
  defines.
- **Started by `srun` or by osoe's own launcher**, with no PMI library; every
  node must reach every other over TCP, the whole mesh, not only the
  rendezvous's port range. A heterogeneous Slurm job is refused at start-up.
- **A host name that resolves only to IPv6 is refused.**
- **At most 65,535 PEs a node**, and **descriptors for the network**: a lead
  holds a socket for every other node and a descriptor for every other PE of its
  node, beside the program's own, so the hard limit on open files (`ulimit -Hn`)
  must leave room for them; `shmem_init` raises the soft limit to it.
- **A program linked statically is refused**: `oshcc -static` says so, since the
  C library's own state would then be gathered with the program's symmetric
  statics.
- **Tested on aarch64.** osoe has been developed and tested on aarch64: the
  Raspberry Pi 5 cluster and an aarch64 virtual machine. On x86-64 the tree of
  28 September, before this one's release fixes, built without warnings and
  passed the one-node tests that can run there - more than 300 in release -
  under Rosetta, an x86-64 user space on an aarch64 kernel, with the futex2
  calls Rosetta refuses stood in for by a shim. Rosetta refuses io_uring, so no
  x86-64 process has sent a message to another node: the network path, and the
  timing pins that were set on aarch64, are untested there, and no x86-64 kernel
  has run any of it.
- **Every node of a job must have the same architecture.** The rendezvous
  compares the job's size, the heap's size, the build's table of message
  handlers, and the collectives' fan-out, team count and pipeline depth, and no
  architecture, so a job that mixed x86-64 and aarch64 nodes would start. Its
  `long double` values would then be wrong with no message - x86-64's 80-bit
  extended format read as aarch64's 128-bit one, and the other way - in
  reductions (`shmem/src/internode.h` says that nothing checks it) and in puts
  and gets of them. Nobody has run such a job.
- **Run with `--cpu-bind=none`.** Slurm binds each task to one core by default
  where a node has as many tasks as cores, and the node's progress thread then
  waits behind its first PE's own thread on that PE's core, which costs a
  pipelined point-to-point pattern most of its rate. It is a trade, since
  unbound tasks make most small collectives and streams within a node slower:
  the numbers are under "Running a program with `srun`".
- **A login that ends on a node just before a job starts there can stall the
  job's start.** systemd-logind deletes a user's files in `/dev/shm` about ten
  seconds after the user's last login session on a machine ends (`RemoveIPC`, on
  by default), and a PE still in `shmem_init` may need a name there that its
  node-mates made. A running job is safe - osoe removes every name once init is
  done - but a start caught in those seconds can wait for ever. Setting
  `RemoveIPC=no` in `logind.conf` on compute nodes, as PostgreSQL's
  documentation also advises, or enabling lingering (`loginctl enable-linger`)
  for the users who run jobs, should prevent it; neither has been tried here.

**Failures**

- **A PE or a node that dies ends the whole job**, with a message naming it.
  There is no fault tolerance.
- **A process that leaves without calling `shmem_finalize` ends the job.** The
  specification has a program end its use of the library with `shmem_finalize`
  or `shmem_global_exit`, and a PE that simply returned from `main` would leave
  the others waiting for it at their next collective. So its exit ends the whole
  job: it says which PE left without finalizing, and every PE ends with status
  1, whatever status the process was returning. A program started with
  `start_pes` is finalized at exit, as the specification says, and is not
  affected. Older programs that never finalized, which some implementations let
  pass, fail here.
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
- **A collective that the PEs of one node call differently is not detected.**
  The leads of the nodes compare a reduction's, a broadcast's or a collect's
  epoch, kind and size - a reduction's operation and element, a broadcast's root
  node - where they meet, and a disagreement between nodes ends the job naming
  it ("PEs called different collectives, or the same one with different
  arguments"). The PEs of one node are compared with nobody: one that
  broadcasts where the others reduce, or passes another count, root or
  `fcollect` size, gets or gives wrong data, and nothing says so - the job can
  end with status 0. The specification leaves a collective that its PEs do not
  call alike undefined.

**Transport and API**

- **TCP only**; no RDMA backend exists (below).
- **One thread a node moves every message between nodes.** The progress thread
  of a node's first PE seals, sends, receives and applies the traffic of all the
  node's PEs, so a node's message rate and its bandwidth to other nodes are one
  thread's, whatever its PE count, and each PE's share of the rate falls as the
  PEs on a node rise. Design §3.12 sketches more such threads for 256 PEs a
  node, sharded by peer node; they are not built. "Put streams run at line rate"
  is a measurement at 1 Gbit/s - 111 to 112 MiB/s on the Pi cluster, a bare
  stream's own rate - and says nothing above it. No faster link has been
  measured. Over loopback between two nodes of a four-core virtual machine
  running nothing else, where only the machine limits a stream, the library
  moved 2.6 to 4.8 GiB/s where a bare io_uring stream between the same two
  processes moved 8.3 to 15.3 - a fifth of it, by the median of each run
  (`docs/design/evidence/release-fixes/floor-idle.txt`). The cost per message of
  one thread serving 256 PEs has not been measured on any machine: the Pi
  cluster's nodes run four PEs each, and the stress tester's sixteen-PE mailbox,
  a pipelined point-to-point pattern, with `--cpu-bind=none`, ran at 256,000
  messages a second over a minute.
- **`shmem_ptr` returns a pointer only for PEs on the same node**, NULL for the
  rest - which the specification allows. A wait is woken by the library's own
  operations - atomics, puts, signals - and a plain store through that pointer
  is none of them: the waiter finds the store only at its 100 ms sweep. Two
  node-mates passing a flag by plain stores took about 115 ms a round trip; by
  `shmem_atomic_set`, 28 µs. The specification has a waited-for word updated by
  an atomic operation (§9.11.1), so this conforms; a program ported from an
  implementation that polls the word should store with `shmem_atomic_set`,
  `shmem_p` or a signal.
- **Strided transfers move element by element, and block-strided ones block by
  block**, batched onto the connection: one message for the whole only when
  the blocks lie end to end.
- **C only.** There are no Fortran bindings. A C++ program can include
  `shmem.h`, builds with `OSOE_CC=g++ oshcc`, and calls the typed routines: the
  type-generic interfaces are C11's `_Generic`, as the specification defines
  them. In C11 the routines whose C11 synopsis gives one name two argument
  lists - `shmem_sync`, `shmem_signal_add`, `shmem_signal_set` - are macros that
  choose by argument count, so a program that declares or defines one by name, a
  profiling wrapper, writes `(shmem_signal_add)` in parentheses.
- **MPI in the same program has not been tried**: the test suite's programs that
  mix the two are not built here.
- **`shmemx.h` is empty.** The cache routines (`shmem_set_cache_inv` and five
  more) were deprecated in 1.3 and left the specification after 1.4; osoe keeps
  them as routines that do nothing, for programs that still call them, which is
  right on a cache-coherent machine, as every one osoe runs on is.

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
  contiguously, so the key table stays 16 bytes a node; reliable connections of
  one of two kinds - XRC queue pairs, one per context and target node, matching
  today's one connection a node pair, which that review recommended, with DC
  behind a build-time switch, or DC (dynamically connected) transport alone,
  which a later review of the seam recommended instead
  (`docs/reviews/archive/phase1-step2-slice1-roce-seam.md`, F12);
  quiet as reaping completions, with a zero-length read where a write's
  completion does not promise visibility; and a per-node polling thread in
  place of the target-side counters RDMA lacks, to wake a PE waiting on a word.
- **Collectives are not out of reach.** A collective's internode phase is behind
  `internode.h`, a seam of its own below the OpenSHMEM layer, and a backend runs
  it there as it can: a tree over RDMA writes and atomics, or a NIC's collective
  engine or an in-network reduction where the fabric has one, handing the rest
  to the generic implementation. Nothing above the header changes, and the
  header says how each call would go. What a RoCE backend would gain first is a
  tree on the fabric in place of today's over TCP.
- **What is not known**: the review's hardware facts are recalled, not
  measured - eight questions stay open, the first of them load-bearing (whether
  a remote initiator can target a lead-registered region through a shared
  XRC domain). XRC against DC is not settled - the first review's V7, DC's
  renegotiation cost, is among the eight questions - and the later review
  counted about 2.1 million XRC queue pairs on one NIC for an all-to-all over
  4,096 nodes of 256 PEs, against a device limit it recalled and did not
  measure, and recommended DC at medium confidence. That is a recommendation
  from recalled limits, not a decision; both lie below the seam, and neither is
  built. The one road to testing a verbs backend without hardware is
  Soft-RoCE (`rdma_rxe`), for function, not performance: post-release work,
  if hardware is still far off then.

## How osoe is tested

- **Four build presets** - `debug`, `release`, AddressSanitizer with
  UndefinedBehaviorSanitizer, and ThreadSanitizer - each with the whole suite
  run before every merge, on the exact tree that merges: more than 650 tests in
  release, and the model checker's 24 tests beside them, which run in that build
  only; fewer under ThreadSanitizer, which cannot see an ordering that runs
  through another process, and so leaves most tests across nodes to the other
  three.
- **Model checking.** Every lock-free and futex protocol but one - waking the
  progress thread, waits, the barrier tree, pipelined collectives, Bruck's
  allgather, the scans, get completion under `SHMEM_THREAD_MULTIPLE`,
  rendezvous, abort, and the proof a deadlock is found by - is checked
  exhaustively in small configurations by an explicit-state model checker
  written for them (`tools/modelcheck`). Six of the models, those of the
  `SHMEM_THREAD_MULTIPLE` designs, run only in a build configured with
  `-DOSOE_MULTIPLE_MODELS=ON`, which no preset sets; get completion's one case
  that a `SHMEM_THREAD_SERIALIZED` program meets runs in every one. Each model
  also carries deliberately broken variants that must fail: a model that cannot
  fail proves nothing. The one without a model is the deprecated active-set
  collectives' (`active-set.c`). The checker runs a model under sequential
  consistency or x86-TSO, and both keep a thread's loads in order, which
  aarch64, the machines osoe is built for, does not: the "blocked in" record's
  model passed while its routine name, then plain characters, could be read torn
  on aarch64. That name, a plain field rewritten while another process reads it,
  was the round-three review's finding K1; it is now four atomic words, and
  `shmem.blocked-record` runs the record's own writer against both of its
  readers on the machine. In an optimized build it fails when the library's
  reader loads the name plainly again; a plain writer, or `osoe-stat`'s reader,
  tore nothing it could see on these machines.
- **Sandia OpenSHMEM's test suite**, on the tree of 28 September: 161 of its 165
  programs passed at 4 × 2 on the Pi cluster, 8 PEs, where `bigget` is run with
  a smaller loop count, since its default is 16 GiB of gets a PE over gigabit
  Ethernet; in the development VM, 159 passed on both of two maps of logical
  nodes. The four that do not are SOS's own: two use an extension of the
  type-generic interfaces and do not build, one overflows its own buffer (osoe
  stops it by name), and one asks `shmem_align` for an alignment the
  specification leaves undefined. Past 61 PEs, `shmem_team_reuse_teams` fails,
  and programs that make a team a PE, such as `reduce_active_set` and
  `repeated_barriers`, skip their last rounds without saying so, on the team
  limits under Limits (temporary, as they are).
- **Applications**: ISx, NPB IS, the Parallel Research Kernels and the
  OpenSHMEM examples, built with `oshcc`.
- **`shmem.floor`'s pins.** In each of seven pairs, in a job of two nodes on one
  machine, `shmem.floor` times the library's get, put with quiet, and fetch-add
  against 8-byte round trips on a bare io_uring TCP connection between the same
  two processes, and the library's stream against a stream on that connection;
  it prints every ratio, each the median of the pairs. Three pins on them - a
  get's, a put-and-quiet's or a fetch-add's fastest round trip at most 3 times
  the connection's; a put-and-quiet's or a fetch-add's median at most 1.5 times
  a get's; the stream at least a tenth of the connection's - are the project's
  regression guard, set from runs on its own machines. They exist only in an
  optimized build without a sanitizer (the `release` preset), and are checked
  only when asked: with `OSOE_TEST_FLOOR_PINS=1` in the environment, or by
  configuring with `-DOSOE_TEST_PINS=ON`, which sets it for `ctest`. Without
  it the test prints its ratios, says it checked no pin, and passes.
  `env.echo-uring`'s own two pins are checked in every such build.
- **A stress tester**, `osoe-pacer`: whole-job patterns from real OpenSHMEM
  programs - a streaming sort, all-to-all exchanges, mailboxes, teams, threads,
  every atomic, transfer and collective, and all of them at once on teams out
  of step - every result checked against an oracle, and every oracle checked
  by a planted defect it must catch. A build of it that counts every call
  (`osoe-pacer-counted`, built where Python 3 is found) shows which of the
  library's 1,794 routines a run reaches.
- **A Raspberry Pi 5 cluster** over gigabit Ethernet, its four nodes running
  with TCP and generic segmentation offload off. Before, one node's spurious
  retransmissions stalled collectives of some sizes for 0.5 to 52 s at a time
  (F1); turning its offload off ended the stalls in the runs that followed,
  though every node had just rebooted, and F1 is not explained - three hours of
  `mix` with the offload off still counted 87 retransmission timeouts
  (`docs/pi-cluster-results.md`, "F1, investigated"). There an 8-byte put with
  quiet, a get or a fetch-add between two nodes took 172 to 176 µs a round trip
  against a bare io_uring TCP echo's 158 to 159, by the medians of seven pairs
  in each of three runs: within 9 to 11% of the echo; put streams run at line
  rate, and a 16-PE barrier takes 176 to 183 µs under Slurm's default binding
  and 196 to 208 µs with the `--cpu-bind=none` that this README recommends
  (`docs/pi-cluster-results.md`).

## Reference

### Environment variables

Unless a row says otherwise, give every PE the same value: `srun` and
`osoe-launch` pass the environment to every task alike. The ones marked
*checked* are compared at start-up between the PEs of a node, which all end, the
one that sees the difference naming the variable. A row marked *checked* alone
is compared between nodes as well, where the node that sees a difference ends
naming the variable and both nodes, the nodes already connected to it end with
it, and a node not yet connected says what it waits for until the launcher ends
it ("How a job starts"); one marked *checked among a node's PEs* is compared
only there. `SHMEM_INFO` prints most of this list from the library itself.

**The specification's** (§8). Each also has a deprecated `SMA_` spelling; when
both are set the `SHMEM_` one controls.

| Variable | Default | What it does |
|---|---|---|
| `SHMEM_SYMMETRIC_SIZE` | `256m` | Symmetric heap bytes a PE, with an optional `k`, `m`, `g` or `t` multiplier; at most 4 TiB. `0` is refused. *Checked*. |
| `SHMEM_VERSION` | unset | Set: every PE prints osoe's version and the specification's at start-up, as `osoe 0.1.0, OpenSHMEM 1.6`. |
| `SHMEM_INFO` | unset | Set: each node's first PE prints the variables osoe reads, with their defaults, under a first line naming osoe's version. |
| `SHMEM_DEBUG` | unset | Set: every PE prints what the library set up for it - its rank and node, its heap's size and address, its control segment's name. |

**Limits**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_MAX_TEAMS` | `64` | Team numbers a job uses between `shmem_init` and `shmem_finalize`, the three predefined teams' included - the world, the node's shared team, and one the library keeps for `shmem_finalize`: `4` to `1024`. A destroyed team keeps its number, so this bounds the teams a job creates - 61 at the default - not only those alive at once (*temporary*: the team work makes it the teams one PE is in at once). Each number costs every PE about 200 KiB of address space at the default fan-out and pipeline depth: 160 KiB that only a team's leads touch and 40 KiB, for the scans, that every member of a team that scans does. *Checked*. |
| `OSOE_MAX_CONTEXTS` | `64` | Contexts a PE may have at once, the default context included: `1` to `4096`. *Checked* among a node's PEs. |
| `OSOE_MAX_THREADS` | `16` | Live threads of a PE that may have called the library, the main thread included: `1` to `1023`. A thread's place is taken at its first call after `shmem_init`, not counting queries such as `shmem_my_pe`, and freed when it exits, not when the call returns; a call that finds none free ends the job, naming the variable. *Checked* among a node's PEs. |
| `OSOE_GET_LANDING_BYTES` | `262144` | Bytes of each PE's landing zone for the replies of its gets from other nodes, a multiple of `1024` up to `67107840`: past it, a new get waits for earlier ones to land. *Checked* among a node's PEs. |

**Collectives**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_COLLECTIVE_FANOUT` | `4` | k of the tree the node leads meet in: `2` to `32`. Over Ethernet it is a bandwidth choice at large sizes, since a node sends a copy to each of its children over its one link. *Checked*. |
| `OSOE_COLLECTIVE_PIPELINE` | `2` | Pieces of a data collective each node keeps in flight up and down that tree: `1` to `8`. *Checked*. |

**Start-up, failures and inspection**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_NO_PROGRESS_TIMEOUT` | `0` | Seconds a wait on the library's own progress - acknowledgements, buffer space, a reply, the rendezvous - may see none before it ends the job, naming what it waited for; `0`, never. A program's own waits and collectives are not timed. |
| `OSOE_KEEPALIVE` | `60,10,6` | `IDLE,INTERVAL,COUNT`: TCP keepalive between nodes, in seconds, seconds and probes. IDLE + INTERVAL × COUNT, 120 s by default, is also how long a node's kernel may leave data sent to it unacknowledged before the node counts as vanished. `0` for neither. |
| `OSOE_DEADLOCK_SECONDS` | `10` | How long a job whose every PE waits in the library, with nothing moving on any node and nothing in flight, is watched before it ends as deadlocked, each node naming where its PEs wait: `1` to `86400`, or `0` for never. |
| `OSOE_PORT_RANGE` | `17000-26999` | `LOW-HIGH`, within `1024-65535`: the ports a node's lead may listen on for the rendezvous. |
| `OSOE_JOB_SECRET` | unset | 32 hexadecimal digits: a secret every node proves it knows when it joins the job, never sent. |
| `OSOE_HEAP_BASE` | chosen | The address every PE maps its heap at, a multiple of the page size, in place of the library's own candidates: a node whose lead finds it taken refuses to start. The node's first PE's value is the node's. |
| `OSOE_SHM_CAPACITY_CHECK` | `1` | `0`: a node whose heaps would not fit `/dev/shm`'s free space is warned, not refused. |
| `OSOE_KEEP_CONTROL_SEGMENT` | `0` | `1`: the control segment keeps its name in `/dev/shm` until finalize, so a crash leaves it for `osoe-stat` to read; `osoe-stat --gc` removes such segments once their PEs are gone, as it does a [failed start's leftovers](#names-a-failed-start-leaves). |

**The progress thread**: each PE's agent thread, which on a node's first PE
moves every message of the node

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AGENT_SLICE_US` | `100` | The scheduler slice each agent thread asks for, in microseconds: `100` to `100000`, or `0` for the kernel's own. A short slice lets it preempt sooner when it wakes on a core it shares - the same fair share, no priority over anyone. Linux 6.12 and later; an older kernel ignores it. It is a trade, measured on the Pi cluster against `0` with 16 PEs on four nodes. Under Slurm's default binding it gave a pipelined mailbox 1.3 to 1.7 times the messages a second, a 64 KiB reduce 24% less time and a 4 KiB alltoall 10% less, and cost the barrier 8% and an 8-byte reduce or broadcast 11 and 13%. Unbound (`--cpu-bind=none`) it left a 64 KiB reduce as it was, gave the mailbox 2% more and the alltoall 8% less time, and cost the barrier 2 to 4% and an 8-byte reduce or broadcast 10 to 12%. A job that fills its nodes gets Slurm's binding without asking, which is why `100` is the default. |
| `OSOE_AGENT_CPUS` | `task` | Where each agent thread may run: `task`, on its own PE's CPUs; `node`, on any CPU any PE of its node was bound to - the job's own cores there, never another's. The PEs' threads keep their binding either way. On the Pi cluster, bound, `node` gave the mailbox a fifth more messages a second where the slice was not in force (`OSOE_AGENT_SLICE_US=0`: 42,900 to 51,300) and nothing measurable beside it (58,500 against 57,500 to 62,900), and beside the slice made the barrier and an 8-byte reduce or broadcast 7 to 15% slower and a 64 KiB reduce 18%. The thread is named `osoe-agent`, as `top -H` and `ps -L` show it. |

**The transport's buffers**, which a program rarely needs to change

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AM_CHUNK` | `65536` | The largest message on a connection: a power of two from `1024` to `1048576`; larger transfers are split. |
| `OSOE_AM_BUFFER` | twice the chunk | Bytes of each connection's buffer: a power of two from twice `OSOE_AM_CHUNK` - the default, and the least allowed: a smaller value is refused, not raised - up to 1 GiB. |
| `OSOE_AM_BUFFER_BUDGET` | `268435456` | Bytes a node may spend on its connections' buffers together - two buffers for each other node. A decimal number of bytes, from twice `OSOE_AM_CHUNK` up to `68719476736` (64 GiB). Where the buffers do not fit, a buffer above its floor, twice the chunk, is halved until they do, and each node's lead says so once. The default buffer is the floor, so at the defaults 1,025 nodes fit and 1,026 are refused: raise the budget or lower the chunk. |
| `OSOE_AM_CREDITS` | `64` | A connection's flow-control credits: `1` to `65536`. |
| `OSOE_AM_HUGEPAGES` | `0` | `1`: map the node's connection buffers with huge pages, falling back, with a warning, when there are none. |
| `OSOE_WIRE_BYTES`, `OSOE_WIRE_BUFFERS` | `16384`, `64` | The size and number of the buffers io_uring receives into, shared by all of a node's connections: the node holds their product, 1 MiB at the defaults, however many peers it has. A size from `64` to `1048576` bytes; a count that is a power of two from `2` to `32768`. |

**For testing**

| Variable | What it does |
|---|---|
| `OSOE_HEAP_SHIFT` | A size: each PE maps its heap that far times its rank past the base, so no two PEs share a symmetric address - how a program is checked for assuming they do. |
| `OSOE_FORCE_REMOTE` | `1`: `shmem_ptr` and `shmem_team_ptr` answer NULL for node-mates, and `SHMEM_TEAM_SHARED` shrinks to the calling PE alone; the traffic to node-mates still goes through shared memory. Needs two logical nodes or more. |
| `OSOE_CHECK_QUIET` | `1`: every quiet on a context that threads share (`SHMEM_THREAD_MULTIPLE`) checks that it completed what it promises, and ends the job naming what it did not. |
| `OSOE_TEST_PERTURB` | `1`: short sleeps and yields at the internal choice points of a context that threads share, to widen races. |
| `OSOE_TEST_NOTICE_SECONDS` | Seconds before a start-up wait's first notice, `1` to `3600`, in place of 30, so that a test sees one without waiting. |
| `OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED` | A rate per 10,000 at which sends and receives are cut short, and the seed that makes a failing pattern repeat. |

**The tools'**: `OSOE_CC` names the compiler `oshcc` runs; `OSOE_NODE_MAP` is
`osoe-launch`'s `-m`; `OSOE_CORPUS` is where `tools/corpus` keeps the reading
corpus it fetches, for development. `OSOE_BOOTSTRAP_TIMEOUT`, `OSOE_WATCHDOG`
and `OSOE_LIVENESS` are retired, and ignored with a notice.

### Starting PEs without a launcher

`srun` and `osoe-launch` set the bootstrap variables below in each PE; the
library reads them as `SLURM_` or `OSOE_` names, whichever set is there
(`OSOE_PROCID` decides). To start one process per PE yourself, set these in
each; a program started with none of them fails at `shmem_init` with "no
launcher environment". There is no single-PE default.

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
| `OSOE_LAUNCH` | Optional, `0` when unset: a number that tells this start from an earlier one by the same job and step ids, as `SLURM_RESTART_COUNT` does under Slurm; `osoe-launch` sets a random one |

- The job id and step id name the job's shared memory
  (`/dev/shm/osoe.<job>.<step>.…`). Two starts with the same pair at once
  collide, and the second fails at init; a job past its start holds no names
  (below), so a later job may use the pair again.
- `OSOE_GTIDS[OSOE_LOCALID]` must equal `OSOE_PROCID`.

For example, two PEs from a shell:

```sh
export OSOE_STEP_NUM_TASKS=2 OSOE_STEP_NUM_NODES=1 OSOE_NODEID=0 OSOE_GTIDS=0,1
export OSOE_JOB_ID=$$ OSOE_STEP_ID=0
OSOE_PROCID=0 OSOE_LOCALID=0 ./my_program &
OSOE_PROCID=1 OSOE_LOCALID=1 ./my_program
wait
```

A start that fails can leave `/dev/shm/osoe.<job>.*` behind
([below](#names-a-failed-start-leaves)): a later start with the same ids takes
over what a process now gone left there, and `osoe-stat --gc` removes it.

### Names a failed start leaves

osoe removes the names it makes during a start - unless `OSOE_KEEP_CONTROL_SEGMENT`
keeps the control segment's - as soon as every PE that needs an object has it
open, so a job that got through its start has none, however it ends. A start
that fails or is cancelled leaves the names it had made by then: a node's
control segment, `osoe.<job>.<step>.<node>.<session>`, and its heaps and
statics (`….heap.<n>`, `….statics.<n>`) until every PE of the node has mapped
the others', and, in a job of more than one node, its job tables and its
connection buffers (`….job.<session>` and `….conn.<session>`) until every PE of
the node has opened them. `<session>` is 0 for a program's first `shmem_init`
and counts up with each initialization after a `shmem_finalize`.

`osoe-stat --gc` removes what such a start left once nothing uses it: a control
segment whose PEs are all gone, with the heaps and statics it names; one no PE
joined, once the process that made it is gone and no live process maps it; the
tombstone a lead that refuses before it has a segment leaves under its name
(below), once no live process maps it; and a node's job tables or connection
buffers whose creating process is gone and which no live process maps. It
leaves alone what a start still under way holds, and an object whose creator
has not yet recorded itself: a start that died in that moment leaves one that
`--gc` cannot tell from a start under way, which stays until a job with the
same ids reclaims it or the epilog below removes it. `osoe-stat --gc` works on the
machine it runs on and removes only what its user may, so a site runs it as
root on each node. `osoe-launch` removes its own job's leftovers itself, and
says which it found, except a control segment `OSOE_KEEP_CONTROL_SEGMENT`
keeps, which it leaves for `--gc`.

Under Slurm a site can also clear a job's names in its epilog. Every object a
job creates in `/dev/shm` is named `osoe.` and the job's number, which is
`SLURM_JOB_ID`:

```sh
rm -f /dev/shm/osoe.${SLURM_JOB_ID}.*
```

### How a job starts

A node's PEs meet in its control segment in `/dev/shm`; then the node leads
meet in a tree and connect their mesh (design §6.2). A failure on the way is
loud, and how far it reaches depends on where it is:

- **A PE that refuses** - its environment, a `/dev/shm` too small for the
  node's heaps - ends by name, and every other PE of its node ends with it,
  saying that another local PE failed: the refusal is published in the node's
  segment, which the lead makes before it checks `/dev/shm`, or, by a lead that
  refuses before it has made one, in a *tombstone* it leaves under the
  segment's name. A PE that refuses while its lead has made no segment waits up
  to 10 s for one to publish in; past that, its lead's notice names it
  instead. A kernel too old for osoe each PE refuses by itself, before
  anything is made.
- **A node whose lead finds the job disagreeing** - another node launched for
  another count, a value the rows above mark *checked* between nodes - ends
  naming the variable and both nodes. The nodes already connected to it in the
  rendezvous tree end with it, having lost it, and the loss travels down the
  tree; a node that has not reached it yet says what it waits for.
- **A task that never reaches the library**, one that dies before
  `shmem_init`, is noticed by nobody: its node's lead waits for it.

Nothing that waits gives up because time has passed (design §3.15). Instead,
once a start-up wait has lasted 30 seconds its node says what it waits for, in
one line and then every minute - the lead for the node, since its other PEs wait
on it, and they end by themselves if its process ends: the lead names the local
PEs that have not joined, and their ranks; a node's first other PE, while its
lead has made no segment, names the lead; a lead calling another node names that
node, its host-list entry, the address and ports it dialled and the last error
("Connection refused", "the connection timed out", "no answer to the hello"); a
lead waiting to be called names the nodes it waits for; and a lead whose
progress thread, created, the system has not yet run says so. The launcher ends
what remains - `srun -K1` at the first task that fails - and
`OSOE_NO_PROGRESS_TIMEOUT` ends a wait that stops moving.

**Trust.** osoe assumes a private cluster network, as Slurm does. With
`OSOE_JOB_SECRET`, a host that does not know the secret cannot join the job:
every lead proves it before it is counted, and nothing a caller claims is acted
on before it has. The secret does not protect the job's start from a host that
can reach the ports and holds connections open - a lead serves sixteen callers
at once, and a full set leaves the rest waiting - nor from a process on a lead's
host that holds a port the lead would answer on and answers first, which a
caller takes for a job launched with another secret and ends; nor the traffic
after the handshake, which is not encrypted or authenticated. Without the
secret, any host that can reach the ports can join.

### How a job ends

A PE that dies, or a node that vanishes, ends the whole job with a failure
(design §3.15). `shmem_global_exit(status)` ends every PE, and the job with
`status`. The calling PE leaves with `exit(status)`, so its `atexit` handlers
run and its streams are flushed, as the specification asks - for at most 10 s, a
constant of the library (`OSOE_ABORT_GRACE_SECONDS` in `shmem/src/abort.h`, not
a variable a job can set), after which the library ends it, with its standard
output flushed unless a thread holds its lock. Every other PE leaves with 0, its
standard I/O flushed and then `_exit`, so its handlers do not run - it is
usually still computing - and output it holds in a buffer of its own, outside
stdio, is lost.

The caller is the only task that leaves nonzero, and it leaves last. A
launcher that ends the step at the first bad exit (`srun -K1`) kills every
task still running then, and a killed task counts 143, which `srun`'s status -
the highest of its tasks' - would then be. So the caller waits until every
other PE has ended before its own end begins: its node's PEs, and its node's
lead the other nodes, each of whose leads waits for its own node first. A PE
whose own end is held - a flush waiting on a lock - ends by its grace, and the
caller after it; only processes held in several places at once, or stopped
outright, can outlast the waits' bounds, the longest 50 s, and with them the
order. `srun` then returns the caller's status, and `osoe-launch`
does the same. When several PEs call it at once, the job's status is one of
theirs, as §9.1.6 asks: on each node the first call carries it, and the other
callers leave with 0 after their handlers.

Two limits remain. A site that sets `WaitTime`, or `srun -W n`, ends the
other tasks n seconds after the first one leaves, whatever its status. And
under `-K1`, two PEs of two nodes that call `shmem_global_exit` at the same
moment each carry their own node's end, and whichever leaves first can still
have Slurm kill a PE of the other node: `srun` may then return 143.
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
so in one line, `SPDX-License-Identifier: LGPL-3.0-or-later`; the four programs
copied from the OpenSHMEM specification's examples, kept as evidence in
`docs/reviews/evidence/phase1-step2-slice1/conformance/`, are the
specification's, not osoe's, and carry its notice instead. A program linked
against the library may carry any license its authors choose; changes to the
library itself are shared under the same terms.
