# osoe

**OpenSHMEM over Ethernet**: an implementation of the OpenSHMEM 1.6
specification for clusters connected by ordinary Ethernet. It needs no RDMA
hardware and no fabric library. It builds from a Linux distribution's own
packages - a C compiler, CMake, pkg-config and liburing - and carries every
operation between nodes over TCP, driven by io_uring; a build for one machine
alone, to develop programs on a desktop, needs only the compiler, CMake and make
([On a desktop](#on-a-desktop-the-single-node-build)). It was developed in an
aarch64 virtual machine and on a cluster of four Raspberry Pi 5 boards, and is
designed to stay usable at hundreds of nodes with 256 processes each - a scale
it has not run at: the largest job across separate machines so far is the Pi
cluster's 16 PEs, four on each node.

**Status.** Every routine of OpenSHMEM 1.6's C interface is implemented, the
deprecated ones of its Annex F included. What departs from the
specification, or takes a choice it leaves open, is listed under
[Limits](#limits-and-where-osoe-departs-from-the-specification). 0.1.0, tagged
`v0.1.0`, is osoe's first release ([its note](docs/release-notes/0.1.0.md)); this
tree has v0.2's work on it since ([the plan](docs/prompts/task-v0.2.md)) and calls
itself 0.1.0 until v0.2 is released. [Installing](#installing) says where the
library and the tools show the version. It has been tested on
aarch64 only: on x86-64 it builds and passes the tests an emulator can run, but
its network path has never run on an x86-64 kernel - there the quick start's
second run, `ctest --preset release` and `osoe-pacer --quick` across nodes are
its first tests. It was written with Claude Code, under its
author's direction ([How osoe was made](#how-osoe-was-made)).

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
# once osoe is installed (Installing); the flags: "Running a program with srun"
oshcc -O2 -o ring ring.c
srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 ./ring
```

## Quick start

On Debian 13 or Raspberry Pi OS Trixie, from the root of this repository, with
the ring above saved there as `ring.c`: install the packages, build osoe, build
the ring with the wrapper that build made, and run it on this machine as four
PEs, and then as four PEs on two logical nodes, which reach each other over TCP
on the loopback as the nodes of a cluster would.

```sh
sudo apt install build-essential cmake pkg-config liburing-dev
cmake --preset release
cmake --build --preset release -j4
build/release/tools/wrapper/oshcc -O2 -o ring ring.c
build/release/tools/launch/osoe-launch -n 4 -- ./ring
build/release/tools/launch/osoe-launch -n 4 -m 0,0,1,1 -- ./ring
```

The build does not run the tests; from a clean tree it took 20 to 21 seconds on
four cores of the development virtual machine. Each run prints these four lines,
in any order - each PE's box holds the number of the PE before it - and exits
with status 0:

```
PE 0 of 4: my box holds 3
PE 1 of 4: my box holds 0
PE 2 of 4: my box holds 1
PE 3 of 4: my box holds 2
```

Four PEs take about 1,090 MiB of `/dev/shm` at the default heap of 256 MiB,
written only as they are used; a machine with less - a Raspberry Pi 5 of 2 GB,
say - refuses the first run by name, with the remedies, and `SHMEM_SYMMETRIC_SIZE=64m`
in front of the command runs it. The second run opens io_uring, so it needs what
[Requirements](#requirements) says of the kernel; `build/release/tools/probe/probe` checks it, a line for each
facility. On a Mac, [Developing on macOS](#developing-on-macos) makes a Linux
virtual machine to run these commands in, with the packages already installed:
`limactl shell osoe` in front of a command runs it there, from the repository
root. [Getting started](#getting-started) has what this leaves out, among it the
test suite, installing, and running over several machines with `srun`.

## On a desktop: the single-node build

A machine that runs jobs of one node only - a desktop with many cores, to
develop and test programs at small PE counts - can build osoe with no liburing
and no io_uring, on a distribution with Linux 4.17 and CMake 3.25: Red Hat 9 and
its rebuilds above all, Debian 12 or Ubuntu 24.04, and Ubuntu 22.04 with a newer
CMake ([Requirements](#requirements); it has run on Rocky Linux 9 and Debian 13,
and in containers on the userlands of Debian 12 and Ubuntu 22.04 to 26.04).
Configure with the option, install, then build the ring above with the installed
wrapper and run it with the installed launcher, from the repository's root -
here as eight PEs, which a machine of four cores runs too, and which need about
2.1 GiB of `/dev/shm`, half the memory of a machine of 4.5 GiB;
`SHMEM_SYMMETRIC_SIZE=64m` in front of the launcher runs them on less:

```sh
cmake --preset release -DOSOE_SINGLE_NODE=ON
cmake --build --preset release -j4
cmake --install build/release --prefix ~/osoe
~/osoe/bin/oshcc -O2 -o ring ring.c
~/osoe/bin/osoe-launch -n 8 -- ./ring
```

`osoe-launch` ends a job that runs past 60 seconds, as a test needs; `-t`
gives a longer one up to a day (`-t 3600`, an hour).

A job of one node behaves the same in both builds; only the system call its
waits sleep in differs - the classic futex there, Linux 6.7's futex2 calls in the
full build - and
every tool's `--version`, `SHMEM_INFO` and the start-up line say
`(single-node build)` after the version. A job of more than one node - under
`srun -N2`, or `osoe-launch -m` with two logical nodes - is refused at
`shmem_init` by name, before anything is made in `/dev/shm`: the full build is
what runs it. `/dev/shm` must hold about 272 MiB a PE at the default heap of 256
MiB, and `SHMEM_SYMMETRIC_SIZE=64m` shrinks that (a container's 64 MiB is too
little: [Building](#building) says what to give one). More PEs than cores is
fine: nothing in the library spins - a waiting PE sleeps on a futex - and in a
job of one node each PE's progress thread has nothing to move and sleeps too, so
the two settings that schedule it in a job of many nodes, `OSOE_AGENT_SLICE_US`
and `OSOE_AGENT_CPUS`, are not needed here. The test suite runs as it does in the
full build, `ctest --preset release -j4`: every test of the full build but those
labelled `internode`, which need more than one node or test the internode
backend's own files, io_uring or futex2, and five of its own ("How osoe is
tested" has the numbers); and `ctest --preset models` runs the same tests of the
model checker, one at a time on a machine of 4 GiB, since the largest wants 3 GiB.

## Getting started

On a Slurm cluster, in this order - each step below has its section; on one
machine alone, [On a desktop](#on-a-desktop-the-single-node-build) is shorter:

1. Build where every node sees the tree, such as a shared home directory
   ([Building](#building)).
2. Check every node: `srun -N<nodes> --ntasks-per-node=1 build/release/tools/probe/probe`
   prints a line a facility, all "ok".
3. Install to a prefix every node sees ([Installing](#installing)).
4. Check the job path: `srun -K1 --cpu-bind=none -N<nodes> --ntasks-per-node=4 osoe-pacer --quick`
   ends `failures=0`.
5. Build a program with `oshcc` and run it with `srun`, or submit the batch
   script ([Running a program with `srun`](#running-a-program-with-srun)).

A problem is worth a report: the line `SHMEM_VERSION=1` prints, the probe's
output on each node, the first error line, and for a hang `osoe-stat`'s.

### Requirements

osoe builds two ways from one tree. The **full build**, the default, runs jobs
of one node or of many; the **single-node build**, `-DOSOE_SINGLE_NODE=ON`
([On a desktop](#on-a-desktop-the-single-node-build)), runs jobs of one node
only, and needs less. The full build:

- A 64-bit system: a 32-bit build is refused when it compiles
- Linux 6.7 or newer, and for a job of more than one node io_uring open to its
  user: the `kernel.io_uring_disabled` sysctl at 0, or at 1 with the user in the
  group `kernel.io_uring_group` names (the probe, `build/<preset>/tools/probe/probe`,
  which ctest runs as `env.probe`, checks)
- liburing 2.6 or newer
- CMake 3.25 or newer, pkg-config, and GCC or Clang: the code is C11 with GNU
  extensions, the symbol attributes and `__typeof__`
- Python 3, optionally: the model checker and the stress tester's counting
  build use it. Where CMake does not find it, it says so while configuring and
  leaves those out; nothing else needs it.

The single-node build:

- A 64-bit system, as above
- Linux 4.17 or newer, for `MAP_FIXED_NOREPLACE` - every distribution's kernel
  in practice - and the futex calls `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET`,
  which every Linux since 2.6.25 has and a sandbox (seccomp) must allow. Before
  5.3, which added `pidfd_open`, and under an emulator without it, each job's
  lead says that it watches its PEs by pid instead
- CMake 3.25 or newer, and GCC or Clang, as above; no liburing and no pkg-config
- Python 3, optionally, as above

osoe has run on aarch64 only, on the Raspberry Pi cluster and in an aarch64
virtual machine. An x86-64 build passed every test an emulator on an aarch64
kernel can run - the full build with a shim standing in for the futex2 calls the
emulator refuses, the single-node build with none, since the emulator passes the
classic futex through - and an aarch64 build made to order memory as x86-64 does
passed the whole suite; no x86-64 kernel has run it: see Limits, "Launching".

Debian 13 (Trixie) and Raspberry Pi OS Trixie provide all of these from their
own repositories - `sudo apt install build-essential cmake pkg-config
liburing-dev`, and `python3` for the models - and a node that only runs jobs
needs only liburing's runtime package, `liburing2`, 2.6 or newer. The full build
does not support older releases, even where the kernel is new enough, since osoe
does not bundle a newer liburing ([design notes](docs/design/shmem-ethernet-design.md) §1 - "design §N"
here names them, and a bare § the OpenSHMEM 1.6 specification): Debian 12 has liburing 2.3 and Ubuntu 24.04
2.5, and Rocky Linux 9.8 turns io_uring off by default and offers no liburing
headers in its enabled repositories, though its 5.14 kernel carries the futex2
calls, backported. Those are what the single-node build is for. On Rocky Linux 9,
where it has run, `sudo dnf install gcc make cmake` - CMake 3.31 at 9.8; 9.0 to
9.2 shipped 3.20, where `pip install cmake` gives a newer one - and on Debian 12,
Ubuntu 22.04 or Ubuntu 24.04 `sudo apt install build-essential cmake`. Ubuntu
22.04's CMake, 3.22, is older than the floor - it answers the preset with
`Unrecognized "version" field` - and `pip install cmake` gives a newer one there
too, with which the single-node build's suite passed whole. The
full build needs a distribution with both a 6.7 kernel and liburing 2.6: Debian
13, Raspberry Pi OS Trixie, or another, such as current Fedora or Arch, or
Ubuntu 24.10 and later.

### Building

Builds use CMake presets and run on Linux only. Each preset builds under
`build/<preset>`, which git ignores:

```sh
cmake --workflow --preset release    # configure, build, and test, without the models
```

That builds one file at a time and runs the tests one at a time - about 860,
checks of the build itself and jobs of a few processes. On four cores of an idle
virtual machine it took about eleven minutes, one for the build and ten for the
tests. Both may run four at a time, as this project's own runs do (`cmake
--build --preset release -j4`, `ctest --preset release -j4`). The 40 tests of
the model checker (`tools/modelcheck`, which needs Python 3) took 52 minutes in
the development VM one at a time, the longest of them, `model.deadlock_owners`,
11 of those, and check the design, which does not vary with the machine. They
want memory more than cores: the largest, `model.abort`, holds 3 GiB at its
peak, the next 2.5 GiB and the one after 1.7 GiB, so run them two at a time on a
machine of 8 GiB and one at a time on 4 GiB - four at once ran the development
VM, of 8 GiB, out of memory.
They have a label of their own, `model`, which the `release` preset leaves out
and the `models` preset runs alone, in the same build:

```sh
ctest --preset models -j2           # the models, once the release build exists (-j1 on 4 GiB)
cmake --workflow --preset models     # or from a clean tree: configure, build, then them, one at a time
```

Presets: `debug`, `release` (optimized, with debug info), `asan`
(AddressSanitizer and UndefinedBehaviorSanitizer), and `tsan` (ThreadSanitizer);
and `tier3`, the release build with tests over network namespaces whose links
`netem` shapes, which needs `sudo` without a password, and `ip`, `tc` and
`ethtool` - the full build's only: with the single-node build it is a configure
error.

The `env.probe` test checks that the running kernel provides every facility
osoe relies on, `shmem_init`'s own checks among them: in the full build every
io_uring and futex2 facility, in the single-node build the classic futex calls,
with the shared-memory facilities in both. A job meets
the compute nodes' kernels, so run it there too, from a build tree every node
sees - it is not installed - as the Pi cluster's runbook does: `srun -N4
--ntasks-per-node=1 build/release/tools/probe/probe`. If a futex check fails, no
job will run there: the full build needs the futex2 calls of Linux 6.7 for a job
of any size, and the single-node build the classic futex calls, which only a
sandbox denies. If only io_uring checks fail - the kernel's `kernel.io_uring_disabled`
sysctl is set, say - a job on one node still runs, since it opens no ring; a job
of more than one node is refused at `shmem_init`, naming the facility that is
missing, for the io_uring facilities `shmem_init` checks (`env.probe` tests a
few more).

io_uring is off by default on RHEL 10 and its rebuilds, and on Rocky Linux 9.8
(`kernel.io_uring_disabled` is 2), and container runtimes' default seccomp
profiles refuse it: Docker 25 and later and containerd 2.0 and later with
`EPERM`, for which the message names the sysctl even though it reads 0 inside
the container, and Podman with `ENOSYS`. There jobs on one node run, but about
410 of the test suite's 867 tests fail at `shmem_init` or at opening a ring -
every one but `env.probe` among the 467 labelled `internode`, which need more
than one node or test the internode backend's own files, io_uring or futex2
(`ctest -L internode`); the single-node build has neither them nor io_uring. Set
the sysctl to 0 as root, or run the container with a seccomp profile that allows
io_uring. Container runtimes also give a container 64 MiB of `/dev/shm` unless
asked for more (Docker's and Podman's `--shm-size`; Kubernetes' pods likewise) -
less than one PE needs at the default heap of 256 MiB - so at their defaults
even a job of one PE is refused at start-up, naming the sizes, and about 680 of
the suite's 867 tests fail, with io_uring refused as well. Give the container a
`/dev/shm` of its own, about 300 MiB for each PE of the node at the default heap, or lower
`SHMEM_SYMMETRIC_SIZE`, rather than the host's (`--ipc=host`), which outlives
the container.

A node's PEs must share one PID namespace and one `/dev/shm`: they meet in
`/dev/shm`, and osoe watches a node's PEs, and judges what a failed start left
there, by process id. Run one container a node with all its PEs inside it, or
containers that keep the host's PID namespace and `/dev/shm` - Apptainer's
default, not its `--pid` or `--containall`. With a PID namespace for each PE, a
PE killed by a signal - a crash, the OOM killer - goes unnoticed where the
namespaces repeat its pid, as identical containers do, and the others wait until
the launcher ends the step (`srun -K1`), or for ever; where the pids differ, a
live PE is taken for dead and the job ends at once; with a
`/dev/shm` for each, the start waits for ever. Where `/dev/shm` outlives a
container (`--ipc=host`), a start killed with its container leaves names that
`osoe-stat --gc` cannot judge, and that make a later start by the same ids fail -
saying another PID namespace left them, where the pids repeat: remove them with
`rm -f /dev/shm/osoe.<job id>.*`.

### Developing on macOS

io_uring is Linux-only, so development on a Mac happens in a Lima VM defined in
this repository. The presets are disabled on macOS and refuse to run there.

```sh
brew install lima
dev/lima/create.sh                                 # Debian 13 VM named osoe
limactl shell osoe cmake --workflow --preset debug # run from the repository root
dev/lima/create.sh osoe-rocky                      # Rocky Linux 9, for the single-node build
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
`osoe-stat`, `osoe-pacer` and `osoe-launch`: all that a node running jobs needs,
a check that osoe works there, and the launcher that runs a job on one machine. The installed programs find the rest from where they are,
so the prefix may be chosen at install time. A program built by `oshcc` loads
the library from the prefix's absolute path, so install under a path that every
node sees the same - a shared file system, or the same install on each - and
give every node liburing's runtime package (the single-node build needs none).
`osoe-bench` stays in the build tree: it is for measurement. Then put the
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
such as `osoe-stat (osoe) 0.1.0`, and the single-node build adds
`(single-node build)` after the version wherever it is printed; the shared library is `libosoe.so.0.1.0`, with
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

`osoe-launch` - `build/<preset>/tools/launch/osoe-launch`, and installed beside
`oshcc` - starts the processes of a job on this machine and sets every bootstrap
variable for them:

```sh
build/release/tools/launch/osoe-launch -n 4 -- ./my_program
build/release/tools/launch/osoe-launch -n 6 -m 0,0,1,1,2,2 -- ./my_program
```

`-m` is a logical node map (design §8.1): one node id per rank, each node's
lead listening on its own loopback address, so one machine runs a job of
several nodes, over TCP between them as a cluster would - up to 1,024
processes on 254 logical nodes; the single-node build refuses such a job by
name, since it has no TCP. `-s` names the variables as `srun` does, so
the job takes the bootstrap's Slurm path. Its logical nodes run without TCP
keepalive unless `OSOE_KEEPALIVE` is set. Their loopback addresses put them in
one network namespace, with no link between them to cut, and the kernel closes
a dead logical node's connections itself, so a probe can find nothing there -
but it can be lost: the kernel's receive backlog drops probes and answers alike
when a machine has more busy threads than cores, and a job of tens of logical
nodes so loaded once ended after two minutes as "stopped answering ... The host
has vanished", where nothing had vanished
(`docs/reviews/archive/v0.2-step4-correctness.md`, Q2-1).

It fails if any process fails, if the timeout (`-t SECONDS`, 1 to 86400, default 60 -
raise it for anything longer than a test)
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
osoe-stat --summary 1234  # the node's collective shape and link, then each team's
                          # barrier or sync between nodes, to walk a hang down
srun --jobid=1234 --overlap -N4 --ntasks-per-node=1 osoe-stat --summary 1234
                          # the same on every node of a Slurm job, as its own user
```

A job that hangs shows, for every PE, the routine it is blocked in, the
collective's epoch - how many collectives its team has begun - and for how long;
and for a node of a job across nodes, each owner of its wires - the lead's
agent and its progress helpers (`OSOE_PROGRESS_HELPERS`) - with the wires it
holds, its passes, its wakes and what it has sent and applied.
The summary's walk follows a team's barriers and syncs between nodes, whose
epochs each node's progress thread keeps; a team hung in a reduction,
broadcast, gather or scan shows there as nothing open, and its PE lines are
where to look. A job of one node - every job of the single-node build - has no
tree between nodes to walk: its summary says so, and the PE lines of
`osoe-stat JOB` name the PE that is not waiting.
The summary names a team you made by its number and its first member's world
rank - `4@r1` - since teams with no PE in common may share a number, and a
blocked PE by its world rank.
A job that deadlocks - every PE waiting in the library, with nothing in flight
between its nodes - ends by itself after `OSOE_DEADLOCK_SECONDS` and prints the
same, node by node (below); a hang the library cannot prove, such as a PE
computing for ever, is what `osoe-stat` is for.

### When a job fails or hangs

- **The exit status says who ended it.** Every PE the library ends leaves with
  1 - a PE or a node that died, a start refused, a deadlock proved, a wait that
  `OSOE_NO_PROGRESS_TIMEOUT` ended. The PE that saw the cause writes a line on
  standard error saying why, and it comes first; each of the others writes a
  line naming the PE of its node that saw the job end, or the node it lost -
  "ended its control wire without its goodbye" of a node whose lead ended -
  and, from a lead with progress helpers, a helper's "ended its connection"
  line beside it. A status
  given to `shmem_global_exit` comes through as it is. Under `srun -K1` a task
  Slurm killed, once another had ended badly, counts 143 (128 plus SIGTERM):
  look for the first message, not the highest status.
- **A start that waits** for its nodes says what for after 30 s, one line a
  node ("How a job starts", below). A task that died before `shmem_init` is waited for until the
  launcher ends the step, or `OSOE_NO_PROGRESS_TIMEOUT`, if it is set, ends the
  wait.
- **A job that hangs**: `osoe-stat JOB` on each of its nodes, and for a job of
  more than one node `osoe-stat --summary JOB` too, above. A deadlock the library can prove ends the job by itself
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

## The repository

What each tracked directory and file at the top of the tree holds. Two
directories that may appear beside them are not in the repository, since
`.gitignore` leaves them out: `build/`, where every preset builds, and
`corpus/`, where `tools/corpus/fetch.sh` puts public OpenSHMEM programs to read.
`base/`, `shmem/`, `tools/`, `dev/` and `docs/` each have a `README.md` of their
own, which says what the directory holds, how it relates to the rest of the
tree, and where to start reading in it; [`docs/README.md`](docs/README.md) says
where to find what among the documents.

| Path | Holds |
|---|---|
| `README.md` | this file |
| `COPYING`, `COPYING.LESSER` | the license texts that [License](#license) names: the GNU General Public License and the Lesser General Public License that supplements it, both version 3 |
| `CMakeLists.txt`, `CMakePresets.json` | the build: the project and its one version, 0.1.0, the option `OSOE_SINGLE_NODE` that makes the single-node build ([On a desktop](#on-a-desktop-the-single-node-build)), and the presets `debug`, `release`, `asan`, `tsan` and `tier3`, with the test presets and workflows over them ([Building](#building)) |
| `.gitignore` | what git ignores: `build/`, `corpus/` and the specification's PDF, among others |
| `CLAUDE.md` | the working agreements every Claude Code session on this repository follows - delegating to subagents, naming, renames, the names of review files ([How osoe was made](#how-osoe-was-made)) |
| `base/` | the pieces the library is made from that need nothing from the OpenSHMEM layer above them, among them futex waits - over futex2 in the full build (`futex-futex2.c`), over the classic futex in the single-node build (`futex-classic.c`) - deadlines, epoch counters, a stall watch, memory fences, and the parsers for settings, Slurm host lists and the node map. An object library folded into `libosoe` and never installed; its tests are in `base/tests/` |
| `shmem/include/` | the public headers, `shmem.h`, `pshmem.h` and `shmemx.h`, and `mpp/`, the historical location, whose headers redirect to those; all installed |
| `shmem/src/` | the library: the OpenSHMEM routines, the runtime under them, `transport.h` and `internode.h` - the boundary a backend implements (design §4.10) - and the one backend below it, in two parts: what every job uses (`transport-am.c`), and the internode backend behind `internode-backend.h`, active messages over TCP and io_uring, which the single-node build replaces with a backend that refuses a job of more than one node (`transport-single-node.c`); `cmake/CheckBoundary.cmake` says which file is which |
| `shmem/tests/` | the library's tests: test programs, shell scripts that start a job and break it to see how it ends, and the Slurm environments the bootstrap is tested against (`slurm-fixtures/`); its `CMakeLists.txt` registers them |
| `tools/` | the programs beside the library, in the next table |
| `cmake/` | CMake modules, and scripts the tests run with `cmake -P` to check the tree itself: that no file above the transport boundary includes one below it, that the library exports only OpenSHMEM's names, that every C11 atomic call names one of the three memory orders the project uses, that the tree builds as strict C11 and as a static archive, and that the single-node build configures from the same lists |
| `dev/` | `lima/`, the definitions of the Debian 13 virtual machine osoe is developed in on a Mac ([Developing on macOS](#developing-on-macos)) and of the Rocky Linux 9 one the single-node build is tested in |
| `docs/README.md` | where to find what in `docs/` |
| `docs/pi-cluster.md`, `docs/pi-cluster-results.md` | the runbook for the four-node Raspberry Pi 5 cluster, and what its sessions measured |
| `docs/release-checklist.md` | what a release needs, ticked where it is done |
| `docs/design/` | the design notes, `shmem-ethernet-design.md`; five proposals, and a note and a proposal on a smaller successor API; virtual traces of reference workloads (`workloads/`); and the raw outputs behind the notes and records (`evidence/`) |
| `docs/decisions/` | the decisions in force, one line each (`README.md`), and the dated records that say who took each one, why, and how to undo it |
| `docs/prompts/` | the written briefs each working session was given; the finished ones are in `done/` |
| `docs/reviews/` | the adversarial reviews: the prompt that runs one, the finished ones (`archive/`), and the raw material of the first two rounds and of two reads of v0.2 step 4's first part (`evidence/`) |
| `docs/reference/` | where to get the OpenSHMEM 1.6 specification, which is not in the repository |

`tools/` has a directory for each program, or group of tests, it builds. Of the
programs, `cmake --install` puts `oshcc`, `osoe-stat`, `osoe-pacer` and
`osoe-launch` under the prefix ([Installing](#installing)); the rest stay in the
build tree.

| `tools/` | Builds | Installed |
|---|---|---|
| `wrapper/` | `oshcc`, the compiler wrapper, and `osoe-statics.ld`, the linker script it passes, which makes a program's global and static variables symmetric | yes, both |
| `stat/` | `osoe-stat`, which reads a running or hung job's counters and state from outside | yes |
| `pacer/` | `osoe-pacer`, the stress tester; in the full build `osoe-pacer-start-pes`, which its tests run to check Annex F's unprefixed routines; and, where Python 3 is found, `osoe-pacer-start-pes-counted` and, in the full build, `osoe-pacer-counted`, builds that count every call to the library | `osoe-pacer` only |
| `launch/` | `osoe-launch`, which starts a job's processes on this machine; beside it `tier3.sh`, which gives each logical node a network namespace, and `capture-slurm-fixtures.sh`, which records Slurm's environment under `srun` | `osoe-launch` only |
| `bench/` | `osoe-bench`, what the library costs on a cluster, one measurement a line | no |
| `probe/` | `probe`, which checks that the kernel has every facility the build relies on - io_uring and futex2 in the full build, the classic futex in the single-node build (the `env.probe` test) | no |
| `echo-uring/` | `echo-uring`, a bare io_uring TCP echo and stream that the library is timed against, and `netem-sweep.sh`, which runs it over emulated links; the full build only | no |
| `modelcheck/` | no program: the model checker, in Python 3, and the models of osoe's protocols, registered as tests in the `release` build where Python 3 is found (six only with `-DOSOE_MULTIPLE_MODELS=ON`) | nothing to install |
| `corpus/` | not part of the build: `fetch.sh` fetches public OpenSHMEM programs, pinned in `manifest.tsv`, into `corpus/` for reading, and `check-traces.py` checks the traces in `docs/design/workloads/traces/` against them | nothing to install |

## Design goals

- **Commodity Ethernet, no fabric offload.** Every operation between nodes is
  carried by the host CPU over TCP. The design notes judge performance against
  a local floor - a bare io_uring TCP echo, a stream, a memcpy - rather than
  against another library's numbers.
- **Built entirely from the distribution's own packages**, the primary goal
  rather than a preference: link-time dependencies are libc and liburing -
  libc alone in the single-node build - and nothing is bundled.
- **Correctness first, reasonable performance second.**
- **Scale with nodes, not with PEs.** The collectives have two levels: the PEs
  of a node meet in shared memory, and then the *nodes* meet. Nothing may scale
  with the number of PEs where it can scale with the number of nodes. Between
  nodes the collectives are logarithmic in the node count, in time and in
  memory - but for the prefix scans, a tree over the team's ranks; alltoall, in
  which every PE has a block for every other; collect's lengths, a word a PE;
  and the deprecated active-set collectives, which run PE by PE - while the
  connection mesh is linear in it, two connections to each of a node's n - 1
  peers ("Scaling with nodes", below); within a node, linear or better in the
  PEs there - tested with at most 15 PEs on a node, and timed with 4 on the Pi
  cluster and, for the barrier alone, up to 32 on one four-core virtual machine
  (design §5.3): a node's barrier arrival is one cache line that all its PEs add
  to, and in a reduction the node's lead PE combines the node's vectors one
  after another on its own thread, O(PEs × bytes).
- **Cores are not assumed spare.** A node runs a PE on every core with the
  library's progress thread beside them, so oversubscription is the normal case:
  nothing spins. A waiting PE sleeps on a futex, and a start-up wait for an
  object not yet made sleeps between looks.
- **Launched by Slurm's `srun`, with no PMI library**: nodes find each other
  over TCP from what the step's environment says.
- **Loud, never hung.** A PE or a node that dies ends the whole job with a
  message naming it, provided the node's PEs share a PID namespace, which
  separate containers can break (Building). No wait gives up merely because time has passed; a job
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
- **Two TCP connections per pair of nodes**, not per pair of PEs. The data
  wire carries every PE's operations between those two nodes as active
  messages; its one ordered stream is what lets `shmem_fence` return without
  waiting: operations to a node are applied in the order they were sent, so a
  fence only asks each node written to since the last acknowledgement to
  acknowledge, and goes on. The control wire carries the barrier's arrivals and
  releases, the deadlock detector's messages and its own goodbye, and nothing
  else, so a barrier's messages never queue behind a put's bytes in a buffer or
  a socket: they share the link, and the node's progress thread, which takes
  the control wires first - or, with progress helpers, not even that: the
  control wires are the lead's agent's and the data wires its helpers'.
- **One progress thread a node drives the network, or a few.** Every PE starts
  an agent thread; once the job has started, the node lead's is the only thread
  that touches a socket or an io_uring ring - or, with `OSOE_PROGRESS_HELPERS`,
  it and that many helper threads of the lead's process, each holding the data
  wires to a share of the other nodes on a ring of its own, the agent keeping
  every control wire. Each sends, receives, and applies arriving puts and
  atomics directly in the target PE's heap - that PE's own threads are never
  interrupted. The other agents do only what must happen inside their own
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
  io_uring with an in-node shared-memory path, which the single-node build
  keeps alone.

## Scaling with nodes, not PEs

The design's rule: between nodes an algorithm is logarithmic in the number of
nodes, n, in time and in memory; within a node it is linear or better in the
PEs of the node, P; and what a node's shared structures and its lead keep to
track the job grows with n, never with N = n × P - every connection, buffer and
table is a node pair's, not a PE pair's - while each PE keeps a few words of
its own for each other node. What breaks the rule is named under the last
heading here. The figures
below are the defaults', at three sizes: the Pi cluster's 4 nodes of 4 PEs, 64
nodes of 64, and the scale osoe is designed for, 500 nodes of 256 PEs (128,000
in all), computed from the code; the times are measured where a record says so.

### Time

| Operation | How | Cost, for n nodes of P PEs, N PEs in all, and a team of M members on S nodes |
|---|---|---|
| put, get, atomic to a node-mate | memcpy or CPU atomic on the shared heap | no system call, but a futex wake when the target waits on the word |
| any of those across nodes | an active message on the node pair's data wire, applied by the target node's progress thread | the connections' state O(n) a node, not O(n × P); each PE's ordering state O(n) a context, 21 bytes a node |
| barrier, sync, and reductions and broadcasts of at most `OSOE_SMALL_COLLECTIVE_BYTES` (1 KiB by default) | the node's PEs meet in shared memory; the node leads meet in a k-ary tree (`OSOE_COLLECTIVE_FANOUT`, default 8), a reduction or broadcast in one piece through slots its team keeps | up a tree at most ⌈log_k n⌉ levels deep and back down, a hop a level each way |
| larger reductions and broadcasts | pieces pipelined through a tree of the node leads whose fan-out - from 2 to `OSOE_COLLECTIVE_FANOUT` - each call takes for its size from the link, measured as the job starts or set (`OSOE_COLLECTIVE_SHAPE`), every node alike, four pieces in flight (`OSOE_COLLECTIVE_PIPELINE`), through slots the thread leading each node's part lends to the collective - its *lane* - which a reduction's leads name to their children as they enter. A reduction the link says it pays for instead takes Träff's circulant reduce-scatter and allgather among the leads (J. L. Träff, 2024): in each of ⌈log₂ n⌉ rounds a half, every lead sends part of its vector to one lead and receives another part from another, combining it in, until each holds its n-th of the result, and then the n-ths go round the other way - through the same lanes, each lead granting its senders its lane as it enters | O(P × bytes) for the lead's combine on a node, then the tree; a reduction's child waits for its parent's lane, once: up to a hop when the two enter together - nothing once the child's own node takes a hop - and up to a round trip, the grant down and the first piece back, when the parent enters after it. On the Pi cluster's four nodes, against 0.1.0's one tree (a fan-out of 4, two in flight), a 1 MiB broadcast took 1.43 to 1.47 times less time and a 1 MiB reduction 1.20 to 1.26, every size from 64 KiB up was faster, and no size slower beyond the run-to-run noise. Träff's: 2⌈log₂ n⌉ rounds, and every lead's link carries under two vectors where a tree's busiest carries k + 1. On the Pi cluster's four nodes, against those trees, a 1 MiB reduction took 1.32 to 1.41 times less time, 2 and 4 MiB 1.43 and 1.46 in one run each at a PE a node, every size from 16 KiB was faster - 1.04 to 1.58 times by OSU's benchmark and the project's probe, though `osoe-bench`'s loop of 64 KiB reductions back to back took as long as the trees' (2.26 ms against 2.24) - and every size to 8 KiB took the trees, unchanged; the cost model, not a measurement, puts the 1 MiB gain at about 1.7 times on seven nodes and more, where a tree has interior nodes |
| `fcollect` | Bruck's allgather among the node leads | ⌈log₂ n⌉ steps, each node receiving every other node's data once |
| `collect` | the PEs' lengths first, as one vector of 8 bytes a PE, summed through the node leads' tree, then the data as `fcollect`'s | the data as `fcollect`'s; the lengths O(N) bytes up the tree and down it, 1 MB at 128,000 PEs |
| prefix scans | a tree over the team's ranks, every member a place in it; a scan of at most `OSOE_SMALL_COLLECTIVE_BYTES` in one piece through slots its team keeps, a larger one pipelined through lanes as the large reductions are, in pieces sized to the scan, at most 16 KiB | O(log N) rounds; a large scan's child waits for its parent's lane, once: about a hop more when they enter together, up to a round trip when the parent enters after it - on the Pi cluster, where such a hop is about 120 µs with its handling, a 2 KiB scan measured 120 to 160 µs slower than through the per-team slots the lanes replaced, and 230 to 260 µs with its root or a member with children late |
| `alltoall`, `alltoalls` | every block put directly, then a barrier | O(N) a PE - inherent: every PE has a block for every other - and so P(N - P) blocks into a node, all applied by its one progress thread, a message each up to a chunk (`OSOE_AM_CHUNK`) and, for an `alltoalls` with a stride, one an element: 32.7 million messages a call at 500 nodes of 256 |
| a team | a number agreed by one reduction over the parent of every member's free numbers, the same on all of them, and a block from each member node's pool, given back at destroy | the reduction of `OSOE_MAX_TEAMS` / 64 + 1 words - about what one broadcast costs - and a second, of one word, when any PE of the parent has `SHMEM_THREAD_MULTIPLE`, then a sync of the parent: about a broadcast and a barrier, and a destroy a barrier of the team; O(P) a member, testing its node's PEs; on each node's lead, O(M log n) once to find the team's nodes from its members, a search of the rank map for each - O(M log M) more when they are not met in node order - and the S leads kept |
| waiting | a futex word per resource a connection has, shared by the node's PEs; one per thread for its replies and watches, per PE for its agent and its landing zone, per context for its counters | O(1) a waiter - but room made in a connection's buffer wakes every writer waiting for it (Limits) |
| start-up | the node leads' rendezvous tree, then their mesh; then, where the large collectives' link is measured, the root's lead timing round trips and streams to its first children's, and one reduction of three words handing every PE its numbers | ⌈log₈ n⌉ levels, then 2(n - 1) connections a lead - a control wire and a data wire a peer - made one after another; then the measurement, about 27 ms of the Pi cluster's start - its streams carry 1.9 MiB, so about 170 ms on a link of 100 Mbit/s - and the reduction's ⌈log_k n⌉ levels at the fan-out k |
| `SHMEM_THREAD_MULTIPLE` | threads issue on a shared context without a lock; every wait that completes its operations - a quiet, `shmem_pe_quiet`, a strided get's end, and the quiet a barrier or `shmem_clear_lock` makes - takes the context's, and one shares a full quiet that began and ended while it waited for it | O(1) an operation |

Where the design admits a limit of its own: a large broadcast, and a reduction
too small for Träff's rounds to pay - under 4 to 16 KiB by the node count on
the Pi cluster's link, by the cost model - go through a tree, whose busiest
lead sends k + 1 vectors where Träff's sends under two; on two nodes, where the
two send the same bytes, the cost model takes the tree for a size of whole
16 KiB pieces and Träff's for most others, not measured either way; Träff's
takes 2⌈log₂ n⌉ rounds where a tree takes 2⌈log_k n⌉ levels, each round a row
of a lane that the fan-out ceiling sizes, so a team of more than 512 nodes
takes the tree at every size at the default ceiling of 8 - a higher
`OSOE_COLLECTIVE_FANOUT` takes Träff's further, to 65,536 nodes at most; a
call's shape is chosen from one link's numbers, the root's lead's to its first
children's, so a job whose links differ is shaped for that one; every pair of
nodes connects twice at start-up, so the job holds n(n - 1) connections, 2(n -
1) a node, and a pair's puts and gets go by one of the two, so a network that
limits or spreads traffic by the connection - a cloud's cap on one flow, a
fabric's hashing of flows to paths - gives a pair one flow's share; and one
thread a node moves all of its internode messages (Limits, below).

Within a node, each collective's phase is O(P): its PEs arrive on one cache
line, the release of a reduction or broadcast is one futex wake of every PE
waiting, and the node's lead combines its PEs' vectors one after another, so a
reduction costs the lead O(P × bytes). A barrier's release on a team that
spans nodes is a chain of wakes O(log P) deep, and on a team of one node one
wake of every PE. Only the one-node barrier is timed past 4 PEs a node: linear
to 32 on a four-core virtual machine, so oversubscribed past 4 - 11 µs at 2 PEs
and 42 at 32 when the machine was quiet (design §5.3), 13 to 18 and 105 to 116
under a review's load. The other phases are timed with 4 PEs a node on the Pi
cluster, and nothing beyond 32.

### Memory

Most of it is address space: an object in `/dev/shm` sized at start but sparse,
or memory allocated and not yet written, which costs a page only once written.
The `/dev/shm` capacity check counts the objects whole all the same.

| What | Where | Grows with | 4 × 4 | 64 × 64 | 500 × 256 |
|---|---|---|---|---|---|
| A PE's heap object: the symmetric heap, then the library's internal region | in `/dev/shm`, one a PE, mapped by every PE of the node | `SHMEM_SYMMETRIC_SIZE`, then about 26 KiB a team number (`OSOE_MAX_TEAMS`) and a 577 KiB lane a thread place (`OSOE_MAX_THREADS` + 1) | 272 MiB, the region 15.63 | 272 MiB, 16.04 | 272 MiB, 16.32 |
| What every PE maps of its node's `/dev/shm` | address space | P heap objects, the control segment and the connection area | 1.06 GiB | 17.1 GiB | 68.4 GiB |
| The control segment | in `/dev/shm`, one a node | about 1.0 MiB, about 287 KiB a PE - 260 KiB of it the PE's landing zone for gets (`OSOE_GET_LANDING_BYTES`), and a 128-byte context block for each context and each thread place (`OSOE_MAX_CONTEXTS`, `OSOE_MAX_THREADS` + 1) - 128 bytes a peer node, and 256 bytes an owner of the node's wires: the lead's agent and each progress helper | 2.1 MiB | 18.9 MiB | 72.8 MiB, 1.5 written at start |
| The connection area | in `/dev/shm`, one a node | 512 KiB of buffers and 1.25 KiB of state a peer node | 1.5 MiB | 31.6 MiB | 250 MiB, 0.6 written at start |
| The node lead's own | its private memory | about 0.8 MiB and 8.1 KB a peer node - the reply pool, and 4.6 KB of it the peer's control wire, 4 KiB its private send buffer - and 1 MiB of receive buffers as traffic comes; with progress helpers, as much again of receive buffers each, and an io_uring ring and a thread's stack | 1.8 MiB | 2.3 MiB | 5.7 MiB |
| Every PE's own, for the other nodes | its private memory | 24 bytes a peer node, and 21 a peer node for each context in use | 135 bytes | 2.8 KB | 22.5 KB |
| The job object: the rank map and each node's address | in `/dev/shm`, one a node | 136 bytes a node, and 24 a run of the rank map - n of them under a block distribution, P under a cyclic one, and under a plane or an arbitrary one as many as the ranks fall in, up to one a rank: 32,000, 768 KB a node, at 500 nodes of 256 under `plane=4` | 1.1 KiB | 10.4 KiB | 79 KiB |
| Sockets | the lead's | 2(n - 1) a node, a control wire and a data wire to each other node; n(n - 1) in the job | 6, 12 | 126, 4,032 | 998, 249,500 |

A team number is 24.25 KiB a PE at 4 nodes and 27.0 at 500: its counters and
small slots - one for each child position of the tree and one for releases, for
its reductions and broadcasts and again for its scans - and its allgather's and
reduce-scatter's words, which grow with log₂ n. A team block is a kilobyte of
the control segment on aarch64 (640 bytes on x86-64), about 760 bytes on the node's lead,
and about 500 bytes of address space on every PE.

**What one more of each setting costs** - a PE's own; every PE of a node maps
all P heap objects there, and the `/dev/shm` capacity check counts them so, P
times these:

| Setting | Each one more |
|---|---|
| `OSOE_MAX_TEAMS` | every PE 24.25 KiB of address space at 4 nodes to 27.4 KiB at a thousand - the allgather's and reduce-scatter's words grow with log₂ n - and 12 bytes of the control segment |
| `OSOE_TEAM_BLOCKS` | the node a kilobyte of `/dev/shm`, written at start; its lead about 760 bytes; every PE about 500 bytes of address space |
| `OSOE_MAX_THREADS` | every PE a 577 KiB lane of address space and 896 bytes of the control segment on aarch64, 768 on x86-64, 128 of them the context block its blocking gets of more than one piece go on |
| `OSOE_MAX_CONTEXTS` | every PE 128 bytes of the control segment and about 180 bytes, and each context in use 21 bytes a peer node |
| `OSOE_PROGRESS_HELPERS` | each node's lead a thread, an io_uring ring and `OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS` of receive buffers, 1 MiB at the defaults; the control segment 256 bytes |
| `OSOE_SMALL_COLLECTIVE_BYTES` | a team number eighteen bytes for each byte of the value, on about 9.0 KiB: 11.3 KiB at 128, 27.0 KiB at the default 1 KiB, 297 KiB at 16 KiB, a PE (at 500 nodes) |
| `OSOE_COLLECTIVE_FANOUT` | a team number 2.5 KiB and a lane 64 KiB, a PE; the tree is at most ⌈log_k n⌉ levels deep - 3 at 500 nodes at the default 8, 5 at 4, 2 at 32 |
| `OSOE_COLLECTIVE_PIPELINE` | a lane 144 KiB, 2.4 MiB a PE across its 17 |
| `OSOE_GET_LANDING_BYTES` | the control segment about that much a PE |
| `OSOE_AM_CHUNK`, `OSOE_AM_BUFFER_BUDGET` | the connection area eight chunks a peer node at the default buffer, four at its floor; the budget then holds the default to budget / (8 × chunk) + 1 nodes, 2,049 at the defaults, and caps the job at budget / (4 × chunk) + 1, 4,097 |
| `SHMEM_SYMMETRIC_SIZE` | the address space every PE maps P times |

### What does not scale, and the limits

- **One progress thread a node**, by default, moves every internode message of
  its node: on the Pi cluster's four PEs a node it keeps up with gigabit's
  bandwidth, and its message rate depends on where it runs - a pipelined
  pattern ran at a sixth of its rate under Slurm's default binding, a core a
  task, against `--cpu-bind=none` (`docs/pi-cluster-results.md`); at 256 PEs a
  node it has not been measured. Progress helpers (`OSOE_PROGRESS_HELPERS`)
  spread a node's data wires over more threads, a share of the other nodes
  each; on four nodes of the Pi cluster they cost broadcasts and alltoalls more
  than they bought, and with many peers on a fast link they have not been
  measured (Limits).
- **`alltoall`** sends P(N - P) blocks into a node, a message each up to a
  chunk and, for an `alltoalls` with a stride, one an element, all applied by
  that thread.
- **`collect`'s lengths** are a vector of N eight-byte words summed through the
  tree - 1 MB a call at 128,000 PEs.
- **Prefix scans** run over a tree of the team's ranks, every member a place in
  it, not over its nodes: O(log N) rounds, a message across nodes wherever a
  rank's neighbour in the tree is on another node.
- **The deprecated active-set collectives** run PE by PE: a binomial tree over
  the set's ranks, log₂ of its size deep, its edges across nodes wherever its
  PEs are.
- **The mesh**: every pair of nodes connects twice at start-up, a control wire
  and a data wire, each lead's 2(n - 1) connections made one after another. A
  lead calls no higher node until every lower one has called it, so the start-up
  is a chain of node-to-node connections, the last lead connected after about 2n
  connection times - modelled at 1.7 ms for 4 nodes and 0.58 s for 1,024 - and a
  larger system may need it changed (`docs/design/mesh-schedule-proposal.md`
  proposes a schedule). On one subnet each node also needs an entry in the
  kernel's neighbour table, its record of a peer's Ethernet address, for each
  of its n - 1 peers, and Linux allows 1,024 by default: for more than about a
  thousand nodes on one subnet, an administrator raises
  `net.ipv4.neigh.default.gc_thresh3` above the node count, and `gc_thresh1`
  and `gc_thresh2` with it.
- **`/dev/shm`**: each PE makes a heap object, which every PE of its node maps,
  so the capacity check asks for P × (`SHMEM_SYMMETRIC_SIZE` + the program's
  statics + 16.3 MiB) and the control segment - 68.1 GiB a node at 256 PEs and
  the default heap, though pages cost only as written - and the connection area
  checks its own, 250 MiB at 500 nodes; lower the heap, or set
  `OSOE_SHM_CAPACITY_CHECK=0`. The landing zones are a little over
  `OSOE_GET_LANDING_BYTES` × P of the segment, 4.25 KiB of header and records a
  zone besides: 65 MiB at 256 PEs.
- **A node's team blocks** run out first at many PEs a node: three whole-job
  `shmem_team_split_2d` grids are as many as the default pool holds at 256 PEs
  a node, and a fourth fails (Limits, "Teams").
- **The ceilings**: 4,097 nodes at the default connection budget (2,049 before
  the buffers halve); 65,535 PEs a node; `OSOE_MAX_TEAMS` 1,024,
  `OSOE_TEAM_BLOCKS` 65,536, `OSOE_MAX_THREADS` 1,023, a fan-out of 32 and 8
  pieces in flight, at most.

## Limits, and where osoe departs from the specification

Everything the specification requires of a C implementation is here. What
follows is every limit a program can meet, marked where it departs from the
specification or takes a choice the specification leaves open.

**Teams**

- **A PE is in at most `OSOE_MAX_TEAMS` teams at once** (256 by default, 1,024
  at most), the three predefined among them - the world, the node's shared team,
  and one the library keeps for `shmem_finalize` - so no PE holds more than 253
  teams of its own. A destroyed team gives its number back to each
  of its members. A split takes, on every member of each team it creates, one
  number free on all of them: `shmem_team_split_strided` one, and a whole-job
  `shmem_team_split_2d` two, every row the one and every column the other. So
  it is what a new team's members hold between them that runs out, not what
  each holds: teams made by splitting one another, or sharing no PE, line their
  numbers up and reach 253 a PE, where teams that overlap otherwise can fail
  sooner, as the order they are made in has it - making every pair of 129 PEs a
  team in a double loop fails at a pair holding 127 and 126, where the same
  pairs made a round of disjoint pairs at a time all succeed - such rounds reach
  253 teams a PE only at 254 PEs. A larger `OSOE_MAX_TEAMS`
  moves the failure out. A split of `SHMEM_TEAM_SHARED` on every node, which is a separate split on each,
  takes one number on each PE, and any number of nodes get their team. With no
  number free on every member a split returns nonzero on every PE of its parent
  team, as the specification allows for a split that cannot be made, and the
  parent's rank 0 says why, naming the variable. Each number costs every PE about
  26 KiB of address space at the default `OSOE_SMALL_COLLECTIVE_BYTES` - its
  counters and the slots of its small collectives and scans, and the
  allgather's and reduce-scatter's words, which grow with the node count's
  logarithm, to about 27 KiB at a thousand nodes - which the `/dev/shm` check
  counts.
- **A node holds at most `OSOE_TEAM_BLOCKS` teams at once** (1,024 by default),
  the three predefined among them, a team of members on several nodes counting
  once on each, and a team of one PE counting too. A split that would make one
  more on a node returns nonzero on every PE of its parent team, each PE whose
  claim of a block there failed saying why, naming the variable and the node. A
  grid of the whole job takes the most: a `shmem_team_split_2d` whose rows are a
  node long or longer gives each of a node's P PEs a column of its own, a block
  each, and with its rows takes P + 1 of the node's blocks - three such grids
  fill the default pool at 256 PEs a node, where the numbers would allow 126 -
  and a team per local rank takes P a family. A block costs a kilobyte of
  `/dev/shm`, allocated at start, and about 760 bytes of memory on the node's
  first PE. A team destroyed before
  the split - on each node, by the PE that would lead the new team there, or by
  PEs of the split's parent that are all of the team's members there - has
  given its place back for it, although a destroy returns before every member
  on the node has let go: the split waits the moment that takes. A team whose
  members on the node are all outside the split's parent, none of them yet
  letting go, can still make the split fail, as a node with no place left
  does.
- **Two splits of different parents at once, on threads of one PE**, under
  `SHMEM_THREAD_MULTIPLE`, can agree the same number. One keeps it and the other
  runs its agreement again, on every PE of its parent; neither fails for it.
  One of the two may wait a moment inside the library for the other's agreement
  to finish on their PE, which waits on nothing outside the split. A split
  takes two reductions over its parent when any PE of the parent has
  `SHMEM_THREAD_MULTIPLE` - the PEs of a job may ask for different levels - and
  one when none has.

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
- **At the defaults a job has at most 4,097 nodes.** A node's connection
  buffers - two for each other node - must fit `OSOE_AM_BUFFER_BUDGET`, 1 GiB.
  The default buffers, 256 KiB each, come to exactly that at 2,049 nodes; from
  2,050 each node's lead halves them to 128 KiB, twice `OSOE_AM_CHUNK`, the
  floor, and prints one line saying so. At the floor the buffers of 4,097 nodes
  come to exactly the budget, and 4,098 nodes are refused at start-up, naming
  it: the budget shrinks the buffers no further, so raise it or lower the
  chunk. A raised `OSOE_AM_BUFFER` shrinks the same way, and sooner. Past 8,192
  nodes, at no helpers, a node's io_uring ring cannot hold its wires, and the
  job ends saying only that the ring could not be made.
- **Global and static variables are symmetric only in a program linked by
  `oshcc`**, which links it without position independence and with osoe's
  linker script. Passing a static where a symmetric address is required in
  any other program ends the job by name. `process_vm_readv` and
  `process_vm_writev`, which reach another process's memory in any program,
  copy between address spaces and map nothing: a put or a get would be a system
  call, an atomic cannot be made of copies, and Yama's `ptrace_scope` of 1
  refuses them between a job's sibling processes. Linux has no call that maps
  memory a process keeps private; gathering the statics by a linker script and
  remapping them as shared memory at init, as osoe does, makes them memory its
  node-mates can map (design §4.3).
- **A PE's gets from other nodes land in its landing zone**
  (`OSOE_GET_LANDING_BYTES`, 256 KiB by default, in extents of 1 KiB): a get
  takes whole extents, at least one, so 256 small gets in flight fill it as 256
  KiB of data does, and past it a new get waits for earlier ones to land -
  backpressure, never an error.
- **A large get from another node is held by its PE's landing zone.** A get's
  pieces of up to 64 KiB - seventeen to a mebibyte - go out together, a
  blocking get's as a nonblocking get's, but no more of them than the landing
  zone holds: four at the default 256 KiB (`OSOE_GET_LANDING_BYTES`). The zone
  is the PE's, shared by its threads and contexts, so while one large get
  fills it another thread's get of any size waits for a piece to land; and the
  pieces in flight toward a node are bounded by its connection's reply credits
  too (`OSOE_AM_CREDITS`, 64, shared by the node's PEs), from sixteen PEs of a
  node getting at once. On the Pi cluster's gigabit a blocking get of 64 KiB
  or more took 1.00 to 1.06 times a put and quiet of the same size, and a
  nonblocking one 0.98 to 1.06; over the development VM's loopback, where a
  round trip's time carries about the zone's 256 KiB, about 2.7 times at a
  mebibyte and 3.3 at 4 MiB. The zone's size for a fast link waits for one
  (`docs/decisions/2026-10-10-blocking-gets.md`;
  `docs/design/blocking-get-proposal.md`, (d)). A thread whose place's
  context block lies past block 4,095, the last a get's token can name - as
  the last places' do from `OSOE_MAX_CONTEXTS` 4,080 at the default
  `OSOE_MAX_THREADS` - gets a piece at a time, a round trip each.
- **A nonblocking fetching atomic completes before it returns.**
  `shmem_atomic_fetch_add_nbi` and its kin issue the same round trip as the
  blocking forms and leave the value in place at once - the earliest the
  specification allows - so between nodes a run of them costs a round trip each,
  rather than overlapping as nonblocking gets do.

**Launching**

- **Linux 6.7 or newer, and the distribution's liburing 2.6 or newer**, for the
  full build; the single-node build needs neither, and runs a job of one node
  only ([Requirements](#requirements)). Other systems are not supported; on a
  Mac, develop in the Lima VM this repository defines.
- **Started by `srun` or by osoe's own launcher**, with no PMI library; every
  node must reach every other over TCP, the whole mesh, not only the
  rendezvous's port range. A heterogeneous Slurm job is refused at start-up.
- **A host name that resolves only to IPv6 is refused.**
- **At most 65,535 PEs a node**, and **descriptors for the network**: a lead
  holds two sockets for every other node - its control wire and its data wire -
  and a descriptor for every other PE of its node, beside the program's own, so
  the hard limit on open files (`ulimit -Hn`) must leave room for them;
  `shmem_init` raises the soft limit to it. On Linux 6.14 and later io_uring
  also charges a node's rings to its user's locked memory, every process of the
  user on the host together, against each one's `ulimit -l` (8 MiB by
  default), zero copy or not: a ring the limit refuses ends the job at
  start-up, naming it.
- **A program linked statically is refused**: `oshcc -static` says so, since the
  C library's own state would then be gathered with the program's symmetric
  statics.
- **Tested on aarch64.** osoe has been developed and tested on aarch64: the
  Raspberry Pi 5 cluster and an aarch64 virtual machine. On x86-64 the tagged
  tree built without warnings and passed every test that can run under Rosetta,
  an x86-64 user space on an aarch64 kernel - 369 of its 718, with a shim
  standing in for the futex2 calls Rosetta refuses; the emulator refused the
  rest io_uring or seccomp - and v0.2 step 1's full build passed its one-node
  tests but four, each failure the emulator's or the cross build's. Rosetta
  refuses io_uring, so no x86-64 process has sent a message to another node: the
  network path, and the timing pins that
  were set on aarch64, are untested there, and no x86-64 kernel has run any of
  it. What x86-64's memory order would do to the protocols across nodes was
  checked instead on aarch64, built to order memory as x86-64 does - stores
  released, loads acquired, a store-buffering litmus test telling the two
  orders apart: the whole suite passed so, the lanes, scan and pipeline tests
  25 times each, and 13 minutes of the stress tester
  (`docs/reviews/archive/phase1-step7-slice4-portability.md`); and again at v0.2
  step 4's merge point, the whole suite with no progress helpers, the tests
  across nodes at two and three, and the stress tester at two and three
  (`docs/reviews/archive/v0.2-step4-portability.md`). The single-node
  build has run on Rocky Linux 9.8 - kernel 5.14, glibc 2.34, GCC 11 - in an
  aarch64 virtual machine, and in the Debian 13 one with liburing hidden from
  it; on the userlands of Debian 12, Ubuntu 22.04 to 26.04 and Fedora 44, in
  containers on Rocky's kernel; and as an x86-64 build under Rosetta with no
  shim, since Rosetta passes the classic futex through: 343 of its 349 tests,
  each failure the emulator's or the cross build's. No x86-64 kernel has run
  it.
- **Every node of a job must have the same architecture.** The rendezvous
  compares the job's size, the heap's size, the build's table of message
  handlers, the collectives' fan-out, team count, pipeline depth, small
  collectives' size and shape, and the link set by hand, and no architecture, so
  a job that mixed x86-64 and aarch64 nodes would start. Its
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

- **A PE or a node that dies ends the whole job**, with a message naming it,
  provided the node's PEs share a PID namespace, which separate containers can
  break (Building). There is no fault tolerance.
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
  flight toward it, and by its kernel's silence when something was - unless
  every node has a loopback address, as `osoe-launch`'s logical nodes do, which
  cannot vanish so and run without either.
- **A program's own deadlock ends the job only once it is proved.** When
  every PE of the job waits inside the library, nothing moves on any node for
  `OSOE_DEADLOCK_SECONDS` (10 by default) and nothing is in flight between
  them, the job ends with status 1, and each node lists where its PEs wait - a
  collective one PE never entered, a wait for a word nobody writes. A PE
  computing for ever, or with any thread of the program's own outside the
  library's waits - a worker, an idle OpenMP pool; osoe's progress helpers do
  not count - is never judged deadlocked, since it may
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
  call alike undefined. Every node's lead decides from the count it is given
  whether a reduction or broadcast is small - at most
  `OSOE_SMALL_COLLECTIVE_BYTES` - or large, which rests on every PE passing the
  same count, as the specification has them do (it says so outright only for
  `alltoall`, `alltoalls` and `fcollect`): where nodes are given counts on
  either side of the line, the job ends where the leads meet, naming both - or,
  under a small reduction's parent, a child taking it as large waits for a
  word its parent never sends, until the deadlock detector ends the job; and
  where they are given counts either side of the size from which the link
  makes a reduction take Träff's, the leads in its rounds and the leads in a
  tree wait for words the others never send, until the deadlock detector ends
  the job. A
  scan's members are compared with their tree parent when the scan is large
  and its team spans nodes - its number among the team's scans, inclusive or
  exclusive, its element and its bytes, the PEs of one node too - and not at
  all when it is small or its team lies on one node, where PEs given other
  counts get wrong results with nothing said; given counts on either side of
  the line, they end the job by name or wait until the deadlock detector ends
  it.
- **Nor is a `shmem_malloc` that the PEs call with different sizes**, which the
  specification leaves undefined too: the next object then starts at another
  place on the PE that differed, puts to it land in the wrong place, and
  nothing says so.
- **Under `srun -K1`, or `osoe-launch` without `-k`, PEs of more than one node
  calling `shmem_global_exit` at once can lose a caller's output**, where the
  specification has every PE's output flushed (§9.1.6): the first node to leave
  can have the launcher end the other nodes' PEs still ending, callers among
  them, whose handlers and buffered output are then lost, and `srun` may return
  143 ("How a job ends"). Call it from one PE.

**Transport and API**

- **TCP only**; no RDMA backend exists (below).
- **By default one thread a node moves every message between nodes.** The
  progress thread of a node's first PE seals, sends, receives and applies the
  traffic of all the node's PEs, so a node's message rate and its bandwidth to
  other nodes are one thread's, whatever its PE count, and each PE's share of
  the rate falls as the PEs on a node rise. `OSOE_PROGRESS_HELPERS` gives the
  lead's process up to 64 more threads, each holding the data wires to a share
  of the other nodes - the peer that is p-th in node order, counting from 0 and
  the node itself left out, is helper 1 + (p mod H)'s - while the lead's agent keeps the control
  wires, so a barrier's messages are one thread's whatever the helpers do. A
  peer's operations stay on one wire and one thread, applied in order, so a
  stream between two nodes is still one thread's: what helpers add is the
  traffic to several peers at once. On the Pi cluster's four nodes of four PEs,
  the one place they have been timed, two helpers made a 64 KiB broadcast and a
  4 KiB alltoall 6 to 35% slower under Slurm's default binding, where a lead's
  threads share its core, and 2 to 9% slower with `--cpu-bind=none`; a 64 KiB
  reduction took 15 to 37% less time bound and about the same unbound
  (`docs/reviews/archive/v0.2-step4-scale-and-performance.md`, S4). What they
  buy with many peers at once on a link faster than one thread has not been
  measured. On the Pi
  cluster's gigabit one thread already runs a stream at the link's rate, and the
  four-core virtual machine the suite runs on has no core to spare for a
  helper. They are built and tested - the suite runs at two and at three
  helpers a node as at none (How osoe is tested) - and cost each node's lead a
  thread, an io_uring ring and a megabyte of receive buffers apiece; by default
  there are none. "Put streams run at line rate"
  is a measurement at 1 Gbit/s - 111 to 112 MiB/s on the Pi cluster, a bare
  stream's own rate - and says nothing above it. No faster link has been
  measured. Over loopback between two nodes of a four-core virtual machine
  running nothing else, the library moved 10.7 to 10.9 GiB/s where a bare
  io_uring stream between the same two processes moved 13.9 to 15.0 - about
  three quarters of it, 0.73 to 0.75 by the median of each run
  (`docs/design/evidence/v0.2-step4/2026-10-08-tip-profile/`), and at v0.2
  step 4's merge point, with the helpers' changes to the progress thread's
  pass, 10.1 to 10.8 GiB/s, 0.71 to 0.76
  (`docs/reviews/archive/v0.2-step4-scale-and-performance.md`, S6). It was a fifth
  before v0.2 step 4 - held back not by the one thread, none in the job more
  than 56% busy, but by the buffer's turnover, one 64 KiB message a buffer
  (`docs/decisions/2026-10-07-one-thread-profile.md`) - two fifths once a put
  filled the room a connection buffer has left, and two thirds with the default
  buffer of 256 KiB. There the receiving node's lead thread became the limit,
  98% busy, and parsing what arrives where the kernel put it, rather than after
  a copy, took the stream to three quarters. That thread still limits one
  stream: its time is the kernel's copy, the TCP stack and the one copy into the
  heap. The cost per message of
  one thread serving 256 PEs has not been measured on any machine: the Pi
  cluster's nodes run four PEs each, and the stress tester's sixteen-PE mailbox,
  a pipelined point-to-point pattern, with `--cpu-bind=none`, ran at 256,000
  messages a second over a minute. And it grows with the PEs of a node waiting
  to send to one other node at once: each reopening of that connection's
  buffer wakes all of them. Until v0.2 step 4 a buffer reopened at the defaults
  had room for one 64 KiB message, so when their messages were that large all
  but one slept again; now a put's part takes the room it finds, so each writer
  of a large put gets some of it, and only a whole message larger than the room
  left sleeps again. In the virtual machine, under the earlier rule, the wakes
  cost the thread a few microseconds a reopening at 32 such PEs, and no run
  showed the node's rate fall; under the present one, and at 256, it has not
  been measured.
- **`shmem_ptr` returns a pointer only for PEs on the same node**, NULL for the
  rest - which the specification allows. A wait is woken by the library's own
  operations - atomics, puts, signals - and a plain store through that pointer
  is none of them: the waiter finds the store only at its 100 ms sweep. Two
  node-mates passing a flag by plain stores took about 115 ms a round trip; by
  `shmem_atomic_set`, 28 µs. The specification has a waited-for word updated by
  an atomic operation (§9.11.1), so this conforms; a program ported from an
  implementation that polls the word should store with `shmem_atomic_set` or a
  signal. A put such as `shmem_p` wakes the waiter too, but on a best effort
  (design §4.9): on x86-64 that wake can be missed, and the 100 ms sweep then
  finds the store.
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
  today's data wire a node pair (the control wire a queue pair of the lead's
  beside them), which that review recommended, with DC behind a build-time
  switch, or DC (dynamically connected) transport alone, which a later review
  of the seam recommended instead
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
  run before every merge, on the exact tree that merges: about 860 tests in
  release, and the model checker's 40 tests beside them, which run in that build
  only; fewer under ThreadSanitizer, which cannot see an ordering that runs
  through another process, and so leaves most tests across nodes to the other
  three.
- **Progress helpers.** The suite runs at `OSOE_PROGRESS_HELPERS=2` and `3` as
  well as at none, by the environment `ctest` passes on to every test - in the
  debug, release and AddressSanitizer presets, and under ThreadSanitizer as
  below. There about a dozen tests across nodes - a different few at each
  count - report a put or an atomic a helper applies in a PE's heap mapping,
  against a local thread's access to the same place or another helper's put
  there in an earlier collective. From under half to three quarters of the
  reports, run by run, are in the words the library keeps for its collectives
  at the end of that mapping, the rest in a program's heap. The ordering that
  makes them safe runs through another process, which the tool cannot see -
  the reason the multi-node maps are left out of that preset (§8.2) - and with
  no helpers the one agent applies every peer's puts and has met the local
  thread through the barrier's handoff, which the tool does see. (Until a
  Träff's reduction's grant gave each receiver a field of its own, two helpers
  could also write its head at once, the same bytes from each, and that word
  was about half of the reports: `docs/decisions/2026-10-10-grant-fields.md`.)
  Every failure of those runs is
  checked to be the tool's reports alone, each in a heap mapping and none in
  the helpers' own words - a check that cannot tell a real race in the
  collectives' words from these, which the models hold instead
  (`docs/reviews/archive/v0.2-step4-correctness.md`, Q2-2). One test, which
  reads back 64 MiB a helper landed, runs past its minute in the tool's
  handling of eight million such reads, and finishes in two given longer. The
  preset gates the suite at no helpers alone, without `shmem.helper-wake`,
  which, like every map of more than one node, the preset leaves out. That
  test counts each owner's wakes - a helper's, and in a second run at no
  helpers the lead's agent's - putting to a peer only once the owner of its
  wire has raised the flag it raises before it sleeps, and checks that the put
  claimed that owner's wake itself: a wake sent to another owner or held back
  fails it, and so does an owner that spins after raising its flag for longer
  than an acknowledgement's round trip, a shorter spin being caught alone but
  less surely under load. Beside four busy loops on four cores it passed 20
  runs of 20, at two helpers and at none, in the release build and under
  AddressSanitizer, where counting wakes without the flag failed 8 of 20 in the
  release build and 9 of 20 under AddressSanitizer
  (`docs/decisions/2026-10-10-helper-wake.md`).
- **Two builds.** The single-node build runs the same suite less the
  467 tests labelled `internode` - those of a job of more than one node, of
  the internode backend's own files, and of io_uring and futex2 - and less
  the full build's check that the single-node build configures, and with
  five of its own, in the same four presets, on Rocky Linux 9 and in the
  development VM with liburing hidden: 399 of the full build's 867 tests in
  the debug preset and its five, two fewer without a C++ compiler, which the
  headers' C++ test needs. `ctest -L internode` on the full build runs the
  tests it lacks but that check, `shmem.single-node.configure`.
- **Model checking.** Every lock-free and futex protocol but one - waking the
  progress thread, waits, the barrier tree, pipelined collectives and the lanes
  their large pieces go through, Bruck's allgather, the scans, get completion
  under `SHMEM_THREAD_MULTIPLE`,
  rendezvous, abort, the proof a deadlock is found by, a progress helper's
  requests and its abort, and the teams' - a team
  block's life, a split's pending numbers, a pool found empty, the posted
  arrivals - is checked
  exhaustively in small configurations by an explicit-state model checker
  written for them (`tools/modelcheck`). Six of the models, those of the
  `SHMEM_THREAD_MULTIPLE` designs, run only in a build configured with
  `-DOSOE_MULTIPLE_MODELS=ON`, which no preset sets; get completion's one case
  that a `SHMEM_THREAD_SERIALIZED` program meets runs in every one. Each model
  also carries deliberately broken variants that must fail: a model that cannot
  fail proves nothing. Some of what the models hold no test of the suite
  reaches: the deadlock detector's use of the helpers' answers - each answer
  naming the request posted last, the helpers' work counted in the node's
  activity, a pass after the request - can each be removed with every detector,
  helper and stress test passing at two helpers, and `deadlock_owners_model.py`
  alone fails without them
  (`docs/reviews/archive/v0.2-step4-correctness.md`, Q2-3). The one without a model is the deprecated active-set
  collectives' (`active-set.c`). The checker runs a model under sequential
  consistency or x86-TSO, and both keep a thread's loads in order, which
  aarch64, the machines osoe is built for, does not: the "blocked in" record's
  model passed while its routine name, then plain characters, could be read torn
  on aarch64. That name, a plain field rewritten while another process reads it,
  was a finding of the review before the first release (`docs/reviews/archive/`);
  it is now four atomic words, and
  `shmem.blocked-record` runs the record's own writer against both of its
  readers on the machine. In an optimized build it fails when the library's
  reader loads the name plainly again; a plain writer, or `osoe-stat`'s reader,
  tore nothing it could see on these machines.
- **Sandia OpenSHMEM's test suite**, on the tree tagged 0.1.0: 161 of its 165
  programs passed at 4 × 2 on the Pi cluster, 8 PEs, where `bigget` is run with
  a smaller loop count, since its default is 16 GiB of gets a PE over gigabit
  Ethernet (2026-10-05); in the development VM, 159 passed on both of two maps
  of logical nodes, on the tree of 28 September. The single-node build, at v0.2
  step 1's review, on one node: 161 of 165 in the development VM, and every C
  program but the same four on Rocky Linux 9. The four that do not are SOS's own: two use an extension of the
  type-generic interfaces and do not build, one overflows its own buffer (osoe
  stops it by name), and one asks `shmem_align` for an alignment the
  specification leaves undefined. At 70 PEs, programs that make a team a round
  and keep them all, such as `reduce_active_set` and `repeated_barriers`, run
  every round: their last PE is in 70 teams, within the 253 of its own a PE may
  hold at the default `OSOE_MAX_TEAMS`. Above 253 PEs they would skip rounds
  again, the programs saying nothing and the library a line for each round,
  naming the variable (Limits).
- **Applications**: ISx, NPB IS, the Parallel Research Kernels and the
  OpenSHMEM examples, built with `oshcc`.
- **`shmem.floor`'s pins.** In each of seven pairs, in a job of two nodes on one
  machine, `shmem.floor` times the library's get, put with quiet, and fetch-add
  against 8-byte round trips on a bare io_uring TCP connection between the same
  two processes, and the library's stream against a stream on that connection;
  it prints every ratio, each the median of the pairs. Three pins on them - a
  get's, a put-and-quiet's or a fetch-add's fastest round trip at most 3 times
  the connection's; a put-and-quiet's or a fetch-add's median at most 1.5 times
  a get's; the stream at least three tenths of the connection's - are the
  project's regression guard, set from runs on its own machines. They exist only
  in an optimized build without a sanitizer (the `release` preset), and are
  checked only when asked: with `OSOE_TEST_FLOOR_PINS=1` in the environment, or by
  configuring with `-DOSOE_TEST_PINS=ON`, which sets it for `ctest`. Without
  it the test prints its ratios, says it checked no pin, and passes.
  `env.echo-uring`'s own two pins are checked in every such build.
- **A stress tester**, `osoe-pacer`: whole-job patterns from real OpenSHMEM
  programs - a streaming sort, all-to-all exchanges, mailboxes, teams, threads,
  every atomic, transfer and collective, and all of them at once on teams out
  of step - every result checked against an oracle, and every oracle checked
  by a planted defect it must catch. A build of it that counts every call
  (`osoe-pacer-counted`, built by the full build where Python 3 is found) shows which of the
  library's 1,794 routines a run reaches.
- **A Raspberry Pi 5 cluster** over gigabit Ethernet, its four nodes running
  with TCP and generic segmentation offload off. Before, one node's spurious
  retransmissions stalled collectives of some sizes for up to 52 s at a time
  (F1); turning its offload off ended the stalls in the runs that followed,
  though every node had just rebooted, and F1 is not explained - three hours of
  `mix` with the offload off still counted 87 retransmission timeouts
  (`docs/pi-cluster-results.md`, "F1, investigated"). There an 8-byte put with
  quiet, a get or a fetch-add between two nodes took 172 to 176 µs a round trip
  against a bare io_uring TCP echo's 158 to 159, by the medians of seven pairs
  in each of three runs: within 9 to 11% of the echo. The ratio does not
  travel: the library adds some 15 µs there, and about 27 over a virtual
  machine's loopback, whose echo takes 27 to 28 µs, so that the same round
  trips take about twice it
  (`docs/design/evidence/release-fixes/floor-idle.txt`) - its cost is the
  machine's more than the link's. Put streams run at line rate, and a 16-PE
  barrier takes 176 to 183 µs under Slurm's default binding and 196 to 208 µs
  with the `--cpu-bind=none` that this README recommends
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
only there. `SHMEM_INFO` prints most of this list from the library itself. In
the single-node build the settings only a job of more than one node uses - the
connections', the wire's, keepalive's, the helpers', the collectives' shape and
the link's - are still checked as values, as the full build checks them in a job
of one node, and change nothing. Every `OSOE_`
setting the library and its tools read is in this section or the table of
"Starting PEs without a launcher" - a test, `shmem.settings-documented`, fails
on one that is not.

**The specification's** (§8). Each also has a deprecated `SMA_` spelling; when
both are set the `SHMEM_` one controls.

| Variable | Default | What it does |
|---|---|---|
| `SHMEM_SYMMETRIC_SIZE` | `256m` | Symmetric heap bytes a PE, with an optional `k`, `m`, `g` or `t` multiplier; at most 4 TiB. `0` is refused. *Checked*. |
| `SHMEM_VERSION` | unset | Set: every PE prints osoe's version and the specification's at start-up, as `osoe 0.1.0, OpenSHMEM 1.6`. |
| `SHMEM_INFO` | unset | Set: each node's first PE prints the variables osoe reads, with their defaults, under a first line naming osoe's version. |
| `SHMEM_DEBUG` | unset | Set: every PE prints what the library set up for it - its rank and node, its heap's size and address, its control segment's name, and in a job of more than one node the link its large collectives choose their shapes from: measured, with what measuring it cost the PE's start, or set. There each node's lead also prints the limit it set on the unsent bytes of its connections (`OSOE_WIRE_UNSENT_BYTES`), once every owner of its wires has applied it, and whether it probes them with TCP keepalive and checks for a silent peer (`OSOE_KEEPALIVE`). |

**Limits**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_MAX_TEAMS` | `256` | Teams one PE may be in at once, the three predefined teams included - the world, the node's shared team, and one the library keeps for `shmem_finalize`: `4` to `1024`. A destroyed team gives its number back. Each number costs every PE about 26 KiB of address space at the other variables' defaults - counters and the slots of its small collectives, which only a team's leads touch, and the same for its scans, which every member of a team that scans does - and 27.4 KiB at a thousand nodes. *Checked*. |
| `OSOE_TEAM_BLOCKS` | `1024` | Teams a node can hold at once, the three predefined teams' included: the node's pool of team blocks in its control segment, a kilobyte each of `/dev/shm`, allocated at start, and about 760 bytes each of the memory of the node's first PE, from which a created team's local lead on each node takes one, and to which the last of its members there gives it back. `8` to `65536`. With none left, a split that needs one on the node returns nonzero on every PE of its parent team, naming the variable and the node. Nodes may differ; a node's PEs may not. *Checked* among a node's PEs. |
| `OSOE_MAX_CONTEXTS` | `64` | Contexts a PE may have at once, the default context included: `1` to `4096`. *Checked* among a node's PEs. |
| `OSOE_MAX_THREADS` | `16` | Live threads of a PE that may have called the library, the main thread included: `1` to `1023`. A thread's place is taken at its first call after `shmem_init`, not counting queries such as `shmem_my_pe`, and freed when it exits, not when the call returns; a call that finds none free ends the job, naming the variable. Each place, and the agent's, has a lane for the large reductions and broadcasts its thread leads and the large scans it makes, 577 KiB of address space at the default fan-out and pipeline depth, which a thread that does neither never touches: 9.6 MiB a PE at the default - but for the link's measurement as a job starts (`OSOE_LINK_HOP_NS`), which writes 64 KiB of the first lane of node 1's first PE and a page of the first lane of each of the next nodes' up to the fan-out. *Checked* among a node's PEs. |
| `OSOE_GET_LANDING_BYTES` | `262144` | Bytes of each PE's landing zone for the replies of its gets from other nodes, a multiple of `1024` up to `67107840`: past it, a new get waits for earlier ones to land. *Checked* among a node's PEs. |

**Collectives**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_COLLECTIVE_FANOUT` | `8` | The ceiling of the tree the node leads meet in: `2` to `32`. A barrier's tree and the small class's take it whole, and a scan's under `fixed`; under `link` a scan's tree keeps 0.1.0's fan-out of 4, or the ceiling where that is less, since a scan cannot change its tree from call to call; a large reduction's or broadcast's takes the fan-out up to it that the link makes cheapest for its size (`OSOE_COLLECTIVE_SHAPE`), since over Ethernet the fan-out is a bandwidth choice at large sizes, a node sending a copy to each of its children over its one link, and a latency choice at small ones. The layout of every team number and lane is sized for it; Träff's reduction takes a lane's rows, at least one a round of a half, so it runs only where ⌈log₂ n⌉ is at most the ceiling + 1 - on up to 512 nodes at the default. *Checked*. |
| `OSOE_COLLECTIVE_PIPELINE` | `4` | The capacity: the most pieces of a large reduction, broadcast or scan each node or member keeps in flight up and down its tree, `1` to `8`, and what every lane's slots are sized for - each round of Träff's reduction keeping that many again for each of a lane's rows its rounds leave it, 16 pieces on three or four nodes and the capacity from 17. A large reduction or broadcast keeps the capacity in flight under `link` as under `fixed`; a scan keeps it under `fixed`, and under `link` 0.1.0's two, or the capacity where that is less: on the Pi cluster four in flight made a scan over 16 PEs on four nodes 1.30 times faster at 1 MiB but 6.6% slower at 64 KiB. A reduction's or a broadcast's piece carries 16 KiB, its header in the lane's slot beside it; a scan's is sized to it, at most 16 KiB. *Checked*. |
| `OSOE_SMALL_COLLECTIVE_BYTES` | `1024` | The largest reduction, broadcast or scan that moves in one piece through slots its team keeps: `128` to `16384`. A larger one goes in pieces through lanes the threads that lead it lend it - every member's thread, for a scan - and a reduction's or scan's child may wait for its parent's, once - up to a hop, or a round trip when the parent enters after it. Each team number keeps the slots - one for each child position of the tree and one for the release, for its reductions and broadcasts and again for its scans - so each byte of the value costs every PE eighteen bytes a number at the default fan-out of 8: lower it for memory, raise it to keep a few kilobytes' reduction or scan in one hop. *Checked*. |
| `OSOE_COLLECTIVE_SHAPE` | `link` | `link`, `tree` or `fixed`: whether a large reduction's or broadcast's shape is to be the one its size makes cheapest on the link - a tree's fan-out and window, or for a reduction Träff's reduce-scatter and allgather - chosen alike on every node from the link's three numbers; the same among the trees alone, Träff's turned off by name; or `OSOE_COLLECTIVE_FANOUT` and `OSOE_COLLECTIVE_PIPELINE` for every call, as osoe 0.1.0 did, which measures nothing - the setting that makes a run reproducible, or 0.1.0's shape at its defaults with `OSOE_COLLECTIVE_FANOUT=4 OSOE_COLLECTIVE_PIPELINE=2`; with no link known, it also leaves each data wire's unsent limit at its 16 KiB floor unless `OSOE_LINK_*` are set (`OSOE_WIRE_UNSENT_BYTES`). A barrier and the small class keep the ceiling's tree under each, and a scan 0.1.0's shape under `link` and `tree` (`OSOE_COLLECTIVE_FANOUT`). *Checked*. |
| `OSOE_LINK_HOP_NS`, `OSOE_LINK_MESSAGE_NS`, `OSOE_LINK_BYTES_PER_SECOND` | not set | The link the shape is chosen from, set by hand in place of its measurement - for tests, and for a fabric known in advance: the one-way time of a small message between two nodes' leads, through both progress threads (`1` to `10000000000` nanoseconds); what a lead's loop spends on each message it sends (`0` to `1000000000` nanoseconds); and the bytes a second a node's link carries each way (`1` to `10000000000000`). All three or none: one alone is refused by name. Not set, a job of more than one node with the shape `link` or `tree` measures the link at the end of `shmem_init` - the root's lead, through the data path with every progress thread running, against its first children's: the hop from round trips, the bandwidth from streams, the message cost from one message to each child against one alone - and every PE takes the root's numbers before `shmem_init` returns. On the Pi cluster that costs a start 27 ms, the hop and the bandwidth agreeing within 8% start to start; the message cost there varies from 0 to 21 µs between starts, which moves no choice its four nodes have. Its streams carry 1.9 MiB, so a slower link pays more - about 170 ms at 100 Mbit/s - and set numbers or the shape `fixed` skip it. Its puts - 136 on two nodes, 198 on four, 278 from nine at the default ceiling - count in the transport counters of the root's lead as a program's would. `osoe-stat` shows the link, measured or set, and so does `SHMEM_DEBUG`'s start-up line. *Checked*. |

**Start-up, failures and inspection**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_NO_PROGRESS_TIMEOUT` | `0` | Seconds a wait on the library's own progress - acknowledgements, buffer space, a reply, the rendezvous - may see none before it ends the job, naming what it waited for; `0`, never. A program's own waits and collectives are not timed. |
| `OSOE_KEEPALIVE` | `60,10,6`, or `0` where every node has a loopback address | `IDLE,INTERVAL,COUNT`: TCP keepalive between nodes, in seconds, seconds and probes. IDLE + INTERVAL × COUNT, 120 s by default, is also how long a node's kernel may leave data sent to it unacknowledged before the node counts as vanished. `0` for neither. Not set, a job whose every node has a loopback address - `osoe-launch`'s logical nodes - has neither: with no link between its nodes to cut, and a kernel that closes a dead node's connections itself and answers for a stopped one, a probe can find nothing there, and an overloaded machine's receive backlog drops them. Nodes with addresses of their own keep both, tier 3's namespaces on one machine among them. `SHMEM_DEBUG` says which a node chose. |
| `OSOE_DEADLOCK_SECONDS` | `10` | How long a job whose every PE waits in the library, with nothing moving on any node and nothing in flight, is watched before it ends as deadlocked, each node naming where its PEs wait: `1` to `86400`, or `0` for never. |
| `OSOE_PORT_RANGE` | `17000-26999` | `LOW-HIGH`, within `1024-65535`: the ports a node's lead may listen on for the rendezvous. |
| `OSOE_JOB_SECRET` | unset | 32 hexadecimal digits: a secret every node proves it knows when it joins the job, never sent. |
| `OSOE_HEAP_BASE` | chosen | The address every PE maps its heap at, a multiple of the page size, in place of the library's own candidates: a node whose lead finds it taken refuses to start. The node's first PE's value is the node's. |
| `OSOE_SHM_CAPACITY_CHECK` | `1` | `0`: a node whose heaps would not fit `/dev/shm`'s free space is warned, not refused. |
| `OSOE_KEEP_CONTROL_SEGMENT` | `0` | `1`: the control segment keeps its name in `/dev/shm` until finalize, so a crash leaves it for `osoe-stat` to read; `osoe-stat --gc` removes such segments once their PEs are gone, as it does a [failed start's leftovers](#names-a-failed-start-leaves). |

**The progress threads**: each PE's agent thread, which on a node's first PE
moves every message of the node, and the helpers that can share the work

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AGENT_SLICE_US` | `100` | The scheduler slice each agent thread asks for, in microseconds: `100` to `100000`, or `0` for the kernel's own. A short slice lets it preempt sooner when it wakes on a core it shares - the same fair share, no priority over anyone. Linux 6.12 and later; an older kernel ignores it. It is a trade, measured on the Pi cluster against `0` with 16 PEs on four nodes. Under Slurm's default binding it gave a pipelined mailbox 1.3 to 1.7 times the messages a second, a 64 KiB reduce 24% less time and a 4 KiB alltoall 10% less, and cost the barrier 8% and an 8-byte reduce or broadcast 11 and 13%. Unbound (`--cpu-bind=none`) it left a 64 KiB reduce as it was, gave the mailbox 2% more and the alltoall 8% less time, and cost the barrier 2 to 4% and an 8-byte reduce or broadcast 10 to 12%. A job that fills its nodes gets Slurm's binding without asking, which is why `100` is the default. |
| `OSOE_AGENT_CPUS` | `task` | Where each agent thread, and each progress helper, may run: `task`, on its own PE's CPUs; `node`, on any CPU any PE of its node was bound to - the job's own cores there, never another's. The PEs' threads keep their binding either way. On the Pi cluster, bound, `node` gave the mailbox a fifth more messages a second where the slice was not in force (`OSOE_AGENT_SLICE_US=0`: 42,900 to 51,300) and nothing measurable beside it (58,500 against 57,500 to 62,900), and beside the slice made the barrier and an 8-byte reduce or broadcast 7 to 15% slower and a 64 KiB reduce 18%. The thread is named `osoe-agent`, as `top -H` and `ps -L` show it. |
| `OSOE_PROGRESS_HELPERS` | `0` | Threads of each node lead's process beside its agent that hold the node's data wires: `0` to `64`. The peer node that is p-th in node order, counting from 0 and leaving the node itself out, is helper 1 + (p mod the count)'s; the lead's agent keeps every control wire, which carry the barrier's and the deadlock detector's messages. Each helper is started after the agent, placed and given a slice as the agents are (`OSOE_AGENT_CPUS`, `OSOE_AGENT_SLICE_US`), named `osoe-helper-N`, and stopped at `shmem_finalize` once it has closed its wires. A peer's traffic is always one thread's, so helpers spread a node's traffic with several peers at once, not one stream; on four nodes of the Pi cluster two of them made broadcasts and alltoalls slower, bound or not, and a 64 KiB reduction faster only under Slurm's binding ("By default one thread a node moves every message between nodes", Limits), and what they buy with many peers on a fast link has not been measured, which is why the default is none. A count above the node's peers leaves the extra helpers idle, each with its ring and receive buffers. Under `OSOE_AGENT_CPUS=task`, the default, the helpers run on the lead PE's CPUs as its agent does: where Slurm binds each task to one core, the lead's main thread, its agent and every helper share that core, and helpers add threads but no CPU - `node` lets them run on any CPU of the node's tasks. `osoe-stat` shows each owner of the node's wires - the agent and every helper - with its wires, passes, wakes and counters; a helper's own thread-scope counters, such as `transport.notify.calls` for the puts it lands, are not kept. Nodes may differ; a node's PEs may not, in a job of more than one node. *Checked* among a node's PEs. |

**The transport's buffers**, which a program rarely needs to change

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AM_CHUNK` | `65536` | The largest message on a connection: a power of two from `1024` to `1048576`; larger transfers are split, and a put's part is cut shorter, down to a sixteenth of the chunk (4,094 bytes at the default), where it fills the room a buffer has left. *Checked* among a node's PEs in a job of more than one node, the refusal naming `OSOE_AM_*`. |
| `OSOE_AM_BUFFER` | four chunks, `262144` | Bytes of each connection's buffer: a power of two from twice `OSOE_AM_CHUNK` - the least allowed: a smaller value is refused, not raised - up to 1 GiB. The default is four chunks, twice the least. *Checked* among a node's PEs in a job of more than one node, as the buffer the budget leaves it, the refusal naming `OSOE_AM_*`. |
| `OSOE_AM_BUFFER_BUDGET` | `1073741824` | Bytes a node may spend on its connections' buffers together - two buffers for each other node. A decimal number of bytes, from twice `OSOE_AM_CHUNK` up to `68719476736` (64 GiB). Where the buffers do not fit, a buffer above its floor, twice the chunk, is halved until they do, and each node's lead says so once. At the defaults the 256 KiB buffers hold to 2,049 nodes, are halved to the floor from 2,050, and 4,097 nodes fit and 4,098 are refused: raise the budget or lower the chunk. *Checked* among a node's PEs in a job of more than one node, as the buffer it leaves, the refusal naming `OSOE_AM_*`. |
| `OSOE_AM_CREDITS` | `64` | A connection's flow-control credits: `1` to `65536`. |
| `OSOE_AM_HUGEPAGES` | `0` | `1`: has no effect yet. It asks for huge pages with `MAP_HUGETLB`, which Linux refuses for the `/dev/shm` object the buffers live in, whatever huge pages are reserved, so each PE of a job of several nodes - which alone has the buffers - warns and maps them as before. (A `/dev/shm` mounted with `huge=always` gives them transparent huge pages either way.) |
| `OSOE_WIRE_BYTES`, `OSOE_WIRE_BUFFERS` | `16384`, `64` | The size and number of the buffers io_uring receives into, shared by the connections of each owner of the node's wires - the lead's agent, and each progress helper: the node holds their product an owner, 1 MiB at the defaults, however many peers it has. A size from `64` to `1048576` bytes; a count that is a power of two from `2` to `32768`. |
| `OSOE_WIRE_ZERO_COPY`, `OSOE_WIRE_ZERO_COPY_BYTES` | `off`, `16384` | `on` sends each connection buffer to the kernel by reference (`IORING_OP_SEND_ZC`) wherever a send carries at least `OSOE_WIRE_ZERO_COPY_BYTES`, and the buffer is not written again until the kernel has let go of it - over TCP, once the peer has acknowledged its bytes - so only one of a connection's two buffers goes by reference at a time, the other copying meanwhile; `off` copies, as before. Off by default, since it loses wherever a copy is cheap: over loopback the kernel copies anyway and adds a notification a send, and a stream ran at about 60% of the copying send's; below about 10 KB, by the kernel's own documentation, pinning the pages costs more than the copy; on the Pi cluster's gigabit a quarter of a stream's sends went by reference and the sending agent's CPU a byte was the copying send's - lending both buffers at once saved a third of it, but left a quiet waiting about half a second at a time for the peer's delayed TCP acknowledgement; and on a fast link a buffer lent is the kernel's until acknowledged, so a peer's stream sends at most one buffer by reference a round trip, the other turning over by copy meanwhile. Not timed on a fast link: v0.2 step 4's last part, on one, decides its default and whether a connection needs more than two buffers. Each send by reference is charged the length it offers, and two pages, until its notification comes, to one count of locked memory for every process of the user on a host, which io_uring holds to each process's own limit (`ulimit -l`; 8 MiB by default): a send the limit refuses goes again by copy, and the connection that met it copies from then on, which a notice naming the limit says once a process - sixteen logical nodes on an emulated gigabit link, each PE putting to every other, met the default, and until this fallback ended "Cannot allocate memory". To keep sending by reference, raise it for the job - `ulimit -l` in its batch script, as far as the hard limit allows. On Linux 6.14 and later io_uring charges its rings to the same count, zero copy or not, so a ring that cannot be made says the limit too. `OSOE_WIRE_ZERO_COPY_BYTES` from `1` up; with it on, a job of more than 3,274 nodes is refused at the defaults with no helpers - the completion queue of the ring that holds the data wires, which more `OSOE_WIRE_BUFFERS` fill sooner and helpers share out. `osoe-stat` counts the sends by reference, a refused one among them, their notifications, and those the kernel did not send by reference after all, a refused one's among them. Each node's own. |

**For testing**

| Variable | What it does |
|---|---|
| `OSOE_HEAP_SHIFT` | A size: each PE maps its heap that far times its rank past the base, so no two PEs share a symmetric address - how a program is checked for assuming they do. |
| `OSOE_FORCE_REMOTE` | `1`: `shmem_ptr` and `shmem_team_ptr` answer NULL for node-mates, and `SHMEM_TEAM_SHARED` shrinks to the calling PE alone; the traffic to node-mates still goes through shared memory. Needs two logical nodes or more. |
| `OSOE_CHECK_QUIET` | `1`: every quiet on a context that threads share (`SHMEM_THREAD_MULTIPLE`) checks that it completed what it promises, and ends the job naming what it did not. |
| `OSOE_TEST_PERTURB` | `1`: short sleeps and yields at the internal choice points of a context that threads share, to widen races. |
| `OSOE_TEST_NOTICE_SECONDS` | Seconds before a start-up wait's first notice, `1` to `3600`, in place of 30, so that a test sees one without waiting. |
| `OSOE_TEST_DEAF_HELPER` | A progress helper's number, `1` up to `OSOE_PROGRESS_HELPERS`: that helper never answers the lead's request to stop, so that a test sees the lead end the job at `shmem_finalize` naming it. Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_TEST_ABORT_DELAY_MS` | Milliseconds, `0` to `5000`: a node acts on a peer's abortive goodbye that late, and each progress helper begins its own abort that long after it sees the node's, so that a test sees what the lead's wait for its helpers' goodbyes is for. Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_WIRE_UNSENT_BYTES` | About the most the kernel may hold unsent on a connection's data wire, which its puts and gets go by (a control wire keeps the system's own): `0` leaves the system's own (`net.ipv4.tcp_notsent_lowat`, no limit by default); otherwise taken as given, however small, up to `1073741824`. Unset, the link's bandwidth times 128 µs, at least 16 KiB - 16 KiB on a gigabit - set at the end of `shmem_init` from the link, measured or set. `OSOE_COLLECTIVE_SHAPE=fixed` measures none, so there it is 16 KiB on any link unless the three `OSOE_LINK_*` give the bandwidth - less than the rule gives a link faster than a gigabit. Uncapped, as at v0.2 step 3, a fetch-add behind a put stream on the Pi cluster waited 17 to 20 ms; at 16 KiB, 3.3 to 3.9 ms, the stream at line rate either way. |
| `OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED` | A rate per 10,000 at which sends and receives are cut short, and the seed that makes a failing pattern repeat. |

**The tools'**: `OSOE_CC` names the compiler `oshcc` runs; `OSOE_NODE_MAP` is
`osoe-launch`'s `-m`; `OSOE_CORPUS` is where `tools/corpus` keeps the reading
corpus it fetches, for development; `OSOE_TEST_PACER_LATE_START`, a positive
number of seconds, starts `osoe-pacer`'s PE 1 that late on the `mix`'s clock,
so that a test sees the PEs agree when its time is up. `OSOE_BOOTSTRAP_TIMEOUT`,
`OSOE_WATCHDOG` and `OSOE_LIVENESS` are retired, and ignored with a notice.

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
| `OSOE_LAUNCH` | Optional, `0` when unset: a number that tells this start from an earlier one by the same job and step ids, as `SLURM_RESTART_COUNT` does under Slurm; `osoe-launch` sets a random one. A start that may follow a failed one by the same ids must set a new one, the same on every PE of the start: PEs given different numbers each wait for the other's launch to go |

- The job id and step id name the job's shared memory
  (`/dev/shm/osoe.<job>.<step>.…`). Two starts with the same pair at once
  collide, and the second fails at init; a job past its start holds no names
  (below), so a later job may use the pair again.
- `OSOE_GTIDS[OSOE_LOCALID]` must equal `OSOE_PROCID`.

For example, two PEs from a shell:

```sh
export OSOE_STEP_NUM_TASKS=2 OSOE_STEP_NUM_NODES=1 OSOE_NODEID=0 OSOE_GTIDS=0,1
export OSOE_JOB_ID=$$ OSOE_STEP_ID=0
export OSOE_LAUNCH=$(date +%s%N)     # anew before every start
OSOE_PROCID=0 OSOE_LOCALID=0 ./my_program &
OSOE_PROCID=1 OSOE_LOCALID=1 ./my_program
wait
```

A start that fails can leave `/dev/shm/osoe.<job>.*` behind
([below](#names-a-failed-start-leaves)): a later start with the same ids and a
new `OSOE_LAUNCH` takes over what a process now gone left there, and
`osoe-stat --gc` removes it. With the same launch number, the later start's PEs
cannot tell the leftover from their own lead's segment, and the start can fail
or wait for ever: set it anew before every start, as above.

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
root on each node - from a timer, not from Slurm's epilog: it reads objects
any user can write, and one written to fail its reading fails the run, which
in an epilog drains the node. The epilog's safe line is the `rm` below.
`osoe-launch` removes its own job's leftovers itself, and
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
  anything is made. A few settings are read only after the node has joined
  the job - `OSOE_DEADLOCK_SECONDS` and `OSOE_AGENT_CPUS` among them - and a
  bad one ends the whole job. Where one PE alone has the bad value, it names
  the variable and every other PE says it ends on behalf of that one; where
  every PE has it, as an exported value gives, some PEs name it and the
  launcher ends the rest without a word.
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
`OSOE_NO_PROGRESS_TIMEOUT` ends a wait that stops moving. The start's last
step, once every node is connected - the link's measurement and the reduction
that hands every PE its numbers - waits as a collective does: a hang there
says nothing at 30 s, and shows in `osoe-stat` as a collective's would, its
routine `shmem_init`.

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
(design §3.15), provided a node's PEs share a PID namespace (Building). `shmem_global_exit(status)` ends every PE, and the job with
`status`. The calling PE leaves with `exit(status)`, so its `atexit` handlers
run and its streams are flushed, as the specification asks. They run on the
library's own thread, not the one that called, with signals blocked: a handler
that reads the caller's thread-local storage - `errno` included - sees that
thread's, and a C++ caller's `thread_local` objects are not destroyed. They run
for at most 10 s, a
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
callers leave with 0, usually after their handlers - a caller whose call loses
the race with another node's end leaves as a PE that did not call.

Two limits remain. A site that sets `WaitTime`, or `srun -W n`, ends the
other tasks n seconds after the first one leaves, whatever its status. And
under `-K1`, when PEs of more than one node call `shmem_global_exit` - as they
all do when each detects the same failure - each node's first caller carries
that node's end, and the first to leave can have Slurm kill the other nodes'
PEs still ending, callers among them, whose handlers and buffered output are
then lost: `srun` may then return 143. To keep the status and the output, call
it from one PE.
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

## How osoe was made

The scope and goals of osoe were first defined by the author and then Claude
Code, Anthropic's agentic coding tool, was used to consider the goals and the
OpenSHMEM specification to generate plans, carry out implementation, and perform
testing on an Arm VM running on macOS and a Raspberry Pi 5 cluster running
Raspberry Pi OS (Trixie). The work was carried out starting in September 2026.
The models were the ones Anthropic offered during September/October 2026:
Claude Opus 5, and from 22 September Claude Opus 5.5, led each working session;
Claude Sonnet 5 and 5.5 took parts of some sessions and of the tasks a session
handed off, and Claude Fable 5.1 did periodic reviews and deep planning. Every
commit names the model that wrote it beside the author, in a `Co-Authored-By`
line, but the first, ten merges and one work-in-progress commit, `de1e2f8`,
kept because the evidence of v0.2 step 2's comparison on the Pi names it, the
rebased form of the commit that comparison ran on
(`docs/design/evidence/pi-cluster/README.md`); and the history is
published whole, as the record of how the work was done.

The work ran from goals down to code, and each level is in the repository:

- **Goals**, the author's: OpenSHMEM 1.6 complete over ordinary Ethernet,
  algorithms logarithmic in the node count, a path to RDMA that no interface
  blocks, and the scale osoe is designed for (design notes §1;
  `docs/decisions/README.md`, "Scope and goals").
- **Planning**, between the author and Claude: sessions that turned the goals
  into the design notes (`docs/design/shmem-ethernet-design.md`) and a plan of
  phases, steps and slices (design §7.1). Every decision is recorded with who took it -
  the author, or Claude where a brief left it to Claude - why, and how to undo it:
  from 23 September the dated records in `docs/decisions/`, and its index of what
  holds; before that, the design notes and the commits.
- **Briefs**: each slice went to a Claude Code session as a written brief - its
  goal, what to read first, what to build, how to verify it, and the questions to
  stop and put to the author (`docs/prompts/`, the finished ones in `done/`).
  `CLAUDE.md` holds the working agreements the sessions followed from 19
  September on.
- **Building**: every lock-free protocol modelled before its code and checked
  exhaustively, each model with broken variants that must fail
  (`tools/modelcheck`); the tests in four build presets, sanitizers among them;
  a speed claim simulated and timed before it merged; `main` moved only at green
  checkpoints, the README true of the tree at every commit.
- **Review**: a fresh Claude agent read changes adversarially before they
  merged - every part of the release fixes and the team redesign among them. At
  seven points - the round-two review onward,
  round one having read the design alone - five more reviewed the whole tree:
  is it OpenSHMEM 1.6, is it correct, does it scale as claimed, does it run
  elsewhere, is it ready to be public; from round three a refutation pass tried
  to knock down every finding rated serious, and a synthesis said what had to
  change (`docs/reviews/archive/`).

The author set the goals, took the decisions the briefs reserved for the
author, answered the questions they raised, and built and cabled the Raspberry
Pi cluster osoe was measured on.

## License

osoe is free software: you can redistribute it and modify it under the terms
of the GNU Lesser General Public License, version 3 or (at your option) any
later version ([COPYING.LESSER](COPYING.LESSER)), which supplements the GNU
General Public License, version 3 ([COPYING](COPYING)). Every source file says
so in one line, `SPDX-License-Identifier: LGPL-3.0-or-later`; the four programs
copied from the OpenSHMEM specification's examples, kept as evidence in
`docs/reviews/evidence/phase1-step2-slice1/conformance/`, are the
specification's, not osoe's, and carry its notice instead - a test,
`shmem.license-lines`, fails on any other that lacks the line. A program linked
against the library may carry any license its authors choose; changes to the
library itself are shared under the same terms.
