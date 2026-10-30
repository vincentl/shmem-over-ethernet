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
it has not run at: the largest jobs across separate machines so far are 256 PEs
(OpenSHMEM's processes, [defined below](#openshmem-in-two-minutes)), 32 on each
of eight Google Cloud nodes, and 512, 256 on each of two, and the Pi cluster's
is 16, four on each node.

**Status.** Every routine of OpenSHMEM 1.6's C interface is implemented, the
deprecated ones of its Annex F included. What departs from the
specification, or takes a choice it leaves open, is listed under
[Limits](#limits-and-where-osoe-departs-from-the-specification). This is
0.2.0, osoe's second release ([its note](docs/release-notes/0.2.0.md);
[the plan](docs/prompts/task-v0.2.md), and [the items](docs/prompts/v0.2-open-items.md)
it closed before its tag). 0.1.0, tagged `v0.1.0`, was the first
([its note](docs/release-notes/0.1.0.md)).
[Installing](#installing) says where the library and the tools show the
version. It has been tested on
aarch64 - the Raspberry Pi cluster and an aarch64 virtual machine - and, since
2026-10-12, natively on x86-64: eight Google Cloud `c4-standard-16` (Debian 13,
Linux 6.12) passed the four build presets (`debug`, `release`, and the
sanitizers `asan` and `tsan`) at no helpers, two and three (progress helpers:
extra threads that share a node's network traffic), tier 3 (tests over network
namespaces), the single-node build and the stress tester `osoe-pacer`, each on a
node by itself, and ran jobs across the nodes over a link of 23 Gbit/s: streams
of puts and five collectives at 512 PEs, 256 on each of two nodes, and on all
eight the stress tester's `mix` at 32 PEs and Sandia OpenSHMEM's test suite at
16 ([the results](docs/fast-link-results.md)). It was written with Claude Code,
under its author's direction ([How osoe was made](#how-osoe-was-made)).

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

The build does not run the tests; from a clean tree it took 29 seconds on four
cores of a development virtual machine. Each run prints these four lines,
in any order - each PE's box holds the number of the PE before it - and exits
with status 0:

```
PE 0 of 4: my box holds 3
PE 1 of 4: my box holds 0
PE 2 of 4: my box holds 1
PE 3 of 4: my box holds 2
```

Four PEs take about 1,130 MiB of `/dev/shm` at the default heap of 256 MiB,
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
2.2 GiB of `/dev/shm`, just under half the memory of a machine of 4.5 GiB;
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
what runs it. `/dev/shm` must hold about 281 MiB a PE at the default heap of 256
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
   prints a line a facility, each "ok" or "info", and ends `PROBE PASSED (0 failures)`.
3. Install to a prefix every node sees ([Installing](#installing)).
4. Check the job path: `srun -K1 --cpu-bind=none -N<nodes> --ntasks-per-node=4 osoe-pacer --quick`
   ends with a line that begins `osoe-pacer failures=0`.
5. Build a program with `oshcc` and run it with `srun`, or submit the batch
   script ([Running a program with `srun`](#running-a-program-with-srun)).

A report of a problem is worth these: the line `SHMEM_VERSION=1` prints
(Installing, below), the probe's output on each node, the first error line,
and for a hang `osoe-stat`'s.

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
  leaves those out; nothing else needs it but the `tsan` preset, whose verdict
  a Python script gives, and which needs CMake 3.28 or newer too
  ([How osoe is tested](#how-osoe-is-tested)).

The single-node build:

- A 64-bit system, as above
- Linux 4.17 or newer, for `MAP_FIXED_NOREPLACE` - every distribution's kernel
  in practice - and the futex calls `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET`,
  which every Linux since 2.6.25 has and a sandbox (seccomp) must allow. Before
  5.3, which added `pidfd_open`, and under an emulator without it, each job's
  lead says that it watches its PEs by pid instead
- CMake 3.25 or newer, and GCC or Clang, as above; no liburing and no pkg-config
- Python 3, optionally, as above

osoe has run on aarch64, on the Raspberry Pi cluster and in an aarch64 virtual
machine, and natively on x86-64: on 2026-10-12 eight Google Cloud
`c4-standard-16` (Intel Xeon Platinum 8581C, Debian 13, Linux 6.12) passed the
four presets at no helpers, two and three - ThreadSanitizer's gate with them -
tier 3, the single-node build and `osoe-pacer`'s runs, each on a node by itself,
and jobs of up to 128 PEs ran across the nodes at 23 Gbit/s; and in two sessions
since, jobs of up to 512 - streams of puts and five collectives at 256 PEs on
each of two nodes - with the stress tester's `mix` at 32 PEs and Sandia
OpenSHMEM's test suite at 16, on all eight nodes
([the results](docs/fast-link-results.md)). Before that, an x86-64 build passed every test an emulator on an aarch64
kernel can run - the full build with a shim standing in for the futex2 calls the
emulator refuses, the single-node build with none, since the emulator passes the
classic futex through - and an aarch64 build made to order memory as x86-64 does
passed the whole suite. The kernels it has run on are Linux 6.12 - Debian 13's,
in the development virtual machines and, as `6.12.111+deb13-cloud-amd64`, on the
Google Cloud nodes - and 6.18, the Pi cluster's; the single-node build has run
on Rocky Linux 9.8's 5.14 besides. No run has been on the 6.7 floor itself,
which is reasoned from the facilities the full build relies on, not run.

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

That builds one file at a time and runs the tests one at a time - 1,214 now,
checks of the build itself and jobs of a few processes. On four cores of an
idle virtual machine it took about thirteen minutes, one for the build and
twelve for the tests (2026-10-17, when they were about 950). Both may run four at a time, as this
project's own runs do (`cmake --build --preset release -j4`, `ctest --preset
release -j4`). The 47 tests of the model checker (`tools/modelcheck`, which
needs Python 3) took 62 minutes in a development VM one at a time on
2026-10-11, when they were 40, the longest of them, `model.reservation`, 14 of
those, and check the design, which does not vary with the machine. They want
memory more than cores: the largest, `model.abort`, holds 3 GiB at its peak,
the next 2.5 GiB and the one after 1.7 GiB, so run them two at a time on a
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
sandbox denies - and `shmem_init` refuses the job, naming the kernel's age
where it is older than 6.7 and a filter where it is not: a container's seccomp
profile older than the calls, a sandbox, or an emulator. If only io_uring checks fail - the kernel's `kernel.io_uring_disabled`
sysctl is set, say - a job on one node still runs, since it opens no ring; a job
of more than one node is refused at `shmem_init`, naming the facility that is
missing, for the io_uring facilities `shmem_init` checks (`env.probe` tests a
few more).

io_uring is off by default on RHEL 10 and its rebuilds, and on Rocky Linux 9.8
(`kernel.io_uring_disabled` is 2), and container runtimes' default seccomp
profiles refuse it: Docker 25 and later and containerd 2.0 and later with
`EPERM`, which the message tells from the sysctl's own refusal by reading the
sysctl - naming a seccomp filter where the sysctl allows io_uring, as it does
inside such a container - and Podman with `ENOSYS`. There jobs on one node run, but about
480 of the 951 tests the suite had on 2026-10-17 failed at `shmem_init` or at
opening a ring - all of them among the 523 then labelled `internode` but
`env.probe`, which need more than one node or test the internode backend's own
files, io_uring or futex2 (`ctest -L internode`, 633 tests now); the
single-node build has neither them nor io_uring. Set
the sysctl to 0 as root, or run the container with a seccomp profile that allows
io_uring. Container runtimes also give a container 64 MiB of `/dev/shm` unless
asked for more (Docker's and Podman's `--shm-size`; Kubernetes' pods likewise) -
less than one PE needs at the default heap of 256 MiB - so at their defaults
even a job of one PE is refused at start-up, naming the sizes, and about 760 of
those 951 tests failed, with io_uring refused as well. Give the container a
`/dev/shm` of its own, about 300 MiB for each PE of the node at the default heap, or lower
`SHMEM_SYMMETRIC_SIZE`, rather than the host's (`--ipc=host`), which outlives
the container.

A node's PEs must share one PID namespace and one `/dev/shm`: they meet in
`/dev/shm`, and osoe watches a node's PEs, and judges what a failed start left
there, by process id - which in another namespace names another process, or
none, so that a PE killed by a signal, a crash or the OOM killer, would go
unnoticed, or a live one be taken for dead. Run one container a node with all
its PEs inside it, or containers that keep the host's PID namespace and
`/dev/shm` - Apptainer's default, not its `--pid` or `--containall`. A node
whose PEs are in more than one PID namespace is refused as it starts, each PE
that gets as far as the check naming two of them and their namespaces; a PE
whose `/proc` cannot say which it is in - not mounted, or another namespace's -
is left out of that comparison. With a `/dev/shm` for each, the start waits for
ever. Where `/dev/shm` outlives a container (`--ipc=host`), a start killed with
its container leaves names that make a later start by the same ids fail, saying
another PID namespace made them, and that `osoe-stat --gc` leaves alone, saying
the same, since neither can judge another namespace's pids: remove them with
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
cmake --install build/release --prefix /opt/osoe    # as root, or a prefix you can write
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
16 PEs. `osoe-pacer --help` lists what else it runs. The smoke test checks data,
and a `shmem_quiet` that returns before its data has landed shows in data only
to a reader on a third node: `osoe-pacer --quick relay`, which needs three
nodes or more, is one.

The version is set once, by `project(osoe VERSION ...)` in `CMakeLists.txt`,
and everything that names a version says it: `SHMEM_VERSION=1` makes every PE
print a line ending `osoe 0.2.0, OpenSHMEM 1.6` at start-up, and `SHMEM_INFO=1`
heads its text with `osoe 0.2.0 environment variables`; `osoe-stat`,
`osoe-pacer`, `osoe-launch` and `osoe-bench` answer `--version` with one line,
such as `osoe-stat (osoe) 0.2.0`, and the single-node build adds
`(single-node build)` after the version wherever it is printed; the shared library is `libosoe.so.0.2.0`, with
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
the kernel chooses, so TCP must be open between the nodes, not only that range -
or, for a firewall that opens ports by range, on a port of `OSOE_MESH_PORT_RANGE`.
A program may finalize and initialize again as often as it likes: a session's
lead listens on its port though the last session's connections still hold it
(`SO_REUSEADDR`), where the 33rd session within a minute once found every one
of its candidate ports taken.
Every node must resolve the names in the step's node list, each name to an
address at which the other nodes reach that node. A lead listens on every
address of its host, and the others find it for the rendezvous at the address
they resolve its name to, so a host with several addresses may have its name
resolve to different ones from node to node. For the connections that follow,
a lead tells the others the address its own host resolves its name to - which
must be one of its own, or the lead refuses to start, naming it - or, where
that is a loopback address, as Debian's `/etc/hosts` makes it, the address its
host would send from toward another node. So osoe uses whichever network those
names resolve to; there is no setting for an interface,
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
  is the one bad one Slurm sees - with the limit under "How a job ends".
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
name, since it has no TCP. On Linux 6.14 and later its logical nodes also
share one count of locked memory, which io_uring charges every node's rings to
against each process's limit - raised by `shmem_init` to the hard one
([What `shmem_init` changes in the process](#what-shmem_init-changes-in-the-process)),
so the hard limit is what counts: at the 8 MiB most distributions give a login
shell, a Raspberry Pi 5 started 112 logical nodes with no helpers, 56 with two
and 42 with three, and refused one more at start-up, naming the limit; an
administrator raises the hard limit for more. The helpers' default counts the
machine, not a logical node: `osoe-launch` gives every rank `OSOE_MACHINE_PES`,
the job's PEs on this machine, and `OSOE_PROGRESS_HELPERS=auto`, the default,
weighs those - all its logical nodes' together - against the machine's CPUs, so
a job whose logical nodes' PEs outnumber its CPUs takes no helpers by default
(the 112 above is what such a job reaches), and three logical nodes of one PE
on a machine of four CPUs take two a node. `-s` names the variables as `srun` does, so
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
memory left behind when no rank reported more (`--help` says the same). A
timeout names the job by its id - the launcher's process id, which `osoe-stat`
takes - and, before ending it, prints where it waits: `osoe-stat --summary`'s
view, from the `osoe-stat` installed beside the launcher.
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
                          # barrier, sync or data collective between nodes, to
                          # walk a hang down
srun --jobid=1234 --overlap -N4 --ntasks-per-node=1 osoe-stat --summary 1234
                          # the same on every node of a Slurm job, as its own user
```

A job that hangs shows, for every PE, the routine it is blocked in, the
collective's epoch - how many collectives its team has begun - and for how
long; and for a node of a job across nodes, each owner of its wires - the
lead's agent and its progress helpers (`OSOE_PROGRESS_HELPERS`; [How osoe
works](#how-osoe-works) says what each is) - with the
wires it holds, its passes and how long ago the last one ended, its wakes and
what it has sent and applied. An idle owner passes ten times a second, so one
whose last pass is more than a fraction of a second old while PEs wait toward
its peers is the one stuck - in a pass, or not running at all. A PE stopped by
SIGSTOP or a debugger is marked `[stopped: SIGSTOP or a debugger]`, since its
record goes on reading as a wait, and the summary counts a node's stopped PEs
beside its longest wait; a job hung in its start shows each PE waiting in
`shmem_init` and the PE of its node, or the node, it waits for. `osoe-stat`
shows every node and every PE of a job on the machine, however many logical
nodes the job has there and however many PEs a node has. The summary's walk
follows a team's barriers and syncs between nodes, whose epochs each node's
progress thread keeps, and its reductions, broadcasts and gathers, whose leads
say on a line of their own under the team's what they wait for and the node to
look at next - a piece from a child, a release from a parent, a round's or a
step's peer - until the walk reaches a node whose lead is not in the
collective, or waits for its own members, whose PE lines say which. Where a
lead waits on one count of several leads' - Träff's grants, the exchange of
collect's totals - its line says how many are missing and names no node, and
the line with nothing open is the node to look at. A scan's members each name
the PE they wait on, in the PE lines. A job of one
node - every job of the single-node build - has no tree between nodes to walk:
its summary says so, and the PE lines of `osoe-stat JOB` name the PE that is
not waiting. The summary names a team you made by its number and its first
member's world rank - `4@r1` - since teams with no PE in common may share a
number, and a blocked PE by its world rank. A job that deadlocks - every PE
waiting in the library, with nothing in flight between its nodes - ends by
itself after `OSOE_DEADLOCK_SECONDS` and prints the same, node by node (below);
a hang the library cannot prove, such as a PE computing for ever, is what
`osoe-stat` is for.

### When a job fails or hangs

- **The exit status says who ended it.** Every PE the library ends leaves with
  1 - a PE or a node that died, a start refused, a deadlock proved, a wait that
  `OSOE_NO_PROGRESS_TIMEOUT` ended. The PE that saw the cause writes a line on
  standard error saying why, and it comes first; each of the others writes a
  line naming the PE of its node that saw the job end, or the node it lost -
  "node 0 (pi1, lead rank 0) ended its control wire without its goodbye" of a
  node whose lead ended, the node by its number, its entry in the step's node
  list and its lead's rank - or "ending: the job is aborting on behalf of PE
  N", saying where it was: waiting in a routine, in `shmem_finalize`, or not
  waiting in the library. Each process says its end once, in the first line any
  of its threads writes - a lead with progress helpers wrote its agent's line
  and a helper's, and a PE its agent's and its own thread's - and does not end
  until that line is out, unless standard error stays blocked for 10 s. It
  names its control segment for `osoe-stat` only where the name is still in
  `/dev/shm` (`OSOE_KEEP_CONTROL_SEGMENT`, or a start that failed), since a
  running job's segment goes with its PEs. A node's number is its place in the
  step's node list, from 0: Slurm's `SLURM_NODEID`, or `osoe-launch`'s logical
  node (`-m`), whose entry is a loopback address. A status
  given to `shmem_global_exit` comes through as it is. Under `srun -K1` a task
  Slurm killed, once another had ended badly, counts 143 (128 plus SIGTERM):
  look for the first message, not the highest status.
- **A start that waits** for its nodes says what for after 30 s, one line a
  node ("How a job starts", below). A task that died before `shmem_init` is waited for until the
  launcher ends the step, or `OSOE_NO_PROGRESS_TIMEOUT`, if it is set, ends the
  wait: the task's own node first, naming its rank, and the nodes that wait on
  that node two seconds later, saying that a node comes to the rendezvous once
  all its PEs have called `shmem_init`.
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
  measured, and with every node's offload off they stayed away a day after
  boot, where with it on shorter stalls came back at the same sizes; why is
  not settled ("How osoe is tested").

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
| `CMakeLists.txt`, `CMakePresets.json` | the build: the project and its one version, 0.2.0, the option `OSOE_SINGLE_NODE` that makes the single-node build ([On a desktop](#on-a-desktop-the-single-node-build)), and the presets `debug`, `release`, `asan`, `tsan` and `tier3`, with the test presets and workflows over them ([Building](#building)) |
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
| `docs/fast-link.md`, `docs/fast-link-results.md`, `docs/fast-link/` | the runbook for the measurements on a link above a gigabit - v0.2 step 4's last part - and what its sessions measured, with its scripts and probes; the first session, on eight x86-64 nodes, was on 2026-10-12, the second on 2026-10-20 and the third on 2026-10-28 |
| `docs/release-checklist.md` | what a release needs, ticked where it is done |
| `docs/release-notes/` | a note for each release: what it is and what changed, what was checked before its tag, and what it leaves for later |
| `docs/ceilings.md` | every ceiling a job can meet, where in the source each is set, and the largest run of each ([Ceilings, and how far each is tested](#ceilings-and-how-far-each-is-tested)) |
| `docs/design/` | the design notes, `shmem-ethernet-design.md`; six proposals, and a note and a proposal on a smaller successor API; virtual traces of reference workloads (`workloads/`); and the raw outputs behind the notes and records (`evidence/`) |
| `docs/decisions/` | the decisions in force, one line each (`README.md`), and the dated records that say who took each one, why, and how to undo it |
| `docs/prompts/` | the written briefs each working session was given; the finished ones are in `done/` |
| `docs/reviews/` | the adversarial reviews: the prompt that runs one, the finished ones (`archive/`), and the raw material of the first two rounds, of two reads of v0.2 step 4's first part and of round eight's scale S4 on the Pi (`evidence/`) |
| `docs/reference/` | where to get the OpenSHMEM 1.6 specification, which is not in the repository |

`tools/` has a directory for each program, or group of tests, it builds. Of the
programs, `cmake --install` puts `oshcc`, `osoe-stat`, `osoe-pacer` and
`osoe-launch` under the prefix ([Installing](#installing)); the rest stay in the
build tree.

| `tools/` | Builds | Installed |
|---|---|---|
| `wrapper/` | `oshcc`, the compiler wrapper, and `osoe-statics.ld`, the linker script it passes, which makes a program's global and static variables symmetric | yes, both |
| `stat/` | `osoe-stat`, which reads a running or hung job's counters and state from outside | yes |
| `pacer/` | `osoe-pacer`, the stress tester; `osoe-pacer-start-pes`, which its tests run to check Annex F's unprefixed routines; and, where Python 3 is found, `osoe-pacer-start-pes-counted` and, in the full build, `osoe-pacer-counted`, builds that count every call to the library | `osoe-pacer` only |
| `launch/` | `osoe-launch`, which starts a job's processes on this machine; beside it `tier3.sh`, which gives each logical node a network namespace, and `capture-slurm-fixtures.sh`, which records Slurm's environment under `srun` | `osoe-launch` only |
| `bench/` | `osoe-bench`, what the library costs on a cluster, one measurement a line; and `collect-places.sh`, which times `collect`'s places by each way at each count of logical nodes with `osoe-bench places` (H24) | no |
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
  which every PE has a block for every other; collect's lengths, a word a
  node, or a word a PE where a node's PEs are not one run of the team's ranks;
  and the deprecated active-set collectives, which run PE by PE - while the
  connection mesh is linear in it, two connections to each of a node's n - 1
  peers ("Scaling with nodes", below); within a node, linear or better in the
  PEs there - tested with up to 64 PEs on a node (the barrier and the
  reductions; the heap, puts and gets, atomics, the lock, the scans and teams
  at 32), and timed with 4 on the Pi
  cluster, 8 to 64 on the Google Cloud one and 256 on two of its nodes, and, for
  the barrier alone, up to 32 on one four-core virtual machine (design §5.3): a
  node's barrier arrival is one
  cache line that all its PEs add to, and in a reduction the node's lead PE
  combines the node's vectors one after another on its own thread,
  O(PEs × bytes).
- **Cores are not assumed spare.** A node runs a PE on every core with the
  library's progress thread beside them, so oversubscription is the normal case:
  nothing spins. A waiting PE sleeps on a futex, and a start-up wait for an
  object not yet made sleeps between looks.
- **Launched by Slurm's `srun`, with no PMI library**: nodes find each other
  over TCP from what the step's environment says.
- **Loud, never hung.** A PE or a node that dies ends the whole job with a
  message naming it; a node whose PEs do not share a PID namespace, as separate
  containers can make it, is refused as it starts (Building). No wait gives up merely because time has passed; a job
  proved deadlocked ends, naming where each PE waits, and a job told to
  (`OSOE_NO_PROGRESS_TIMEOUT`) ends a wait on the library's own progress that
  has stopped moving, and says what it waited for. Loud stops where the library
  cannot see. With `OSOE_CHECK_COLLECTIVES=0` the PEs of one node are not
  compared with each other on the collectives they call, so a collective they
  call differently goes unnamed; by default they are, and it is named (Limits,
  "Failures").
- **Observable from outside**: `osoe-stat` reads a running or hung job's
  counters and state from its control segment, without its cooperation.
- **A path to RDMA that no interface blocks.** The transport and collective
  seams are designed so that an RDMA (RoCE) backend could replace TCP at
  hundreds of nodes and toward 10⁶ PEs - a constraint on the design, not a
  backend this project builds (below).

## How osoe works

A label in parentheses - (H15), (P4), (T35) - names the item of v0.2's open
list where that work was asked for and closed
([`docs/prompts/v0.2-open-items.md`](docs/prompts/v0.2-open-items.md), which
gives each one's record); the rest of the README cites them the same way.

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
  control wires are the lead's agent's and the data wires its helpers'. Behind
  a stream at Google Cloud's link rate a barrier took 499 µs against 99 alone
  at no helpers, and 121 against 94 with two (H15, 2026-10-28).
- **One progress thread a node drives the network, or a few.** Every PE starts
  an agent thread; once the job has started, the node lead's is the only thread
  that touches a socket or an io_uring ring - or, with `OSOE_PROGRESS_HELPERS`
  (two by default on a job of three nodes or more where a node's PEs fit its
  CPUs), it and that many helper threads of the lead's process, each holding the
  data wires to a share of the other nodes on a ring of its own, the agent
  keeping every control wire. Each sends, receives, and applies arriving puts and
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
| larger reductions and broadcasts | pieces pipelined through a tree of the node leads whose fan-out - from 2 to `OSOE_COLLECTIVE_FANOUT` - each call takes for its size from the link, measured as the job starts or set (`OSOE_COLLECTIVE_SHAPE`), every node alike, eight pieces in flight (`OSOE_COLLECTIVE_PIPELINE`; four until 2026-10-28, the window of the Pi cluster's timings below), through slots the thread leading each node's part lends to the collective - its *lane* - which a reduction's leads name to their children as they enter. A reduction the link says it pays for instead takes Träff's circulant reduce-scatter and allgather among the leads (J. L. Träff, 2024): in each of ⌈log₂ n⌉ rounds a half, every lead sends part of its vector to one lead and receives another part from another, combining it in, until each holds its n-th of the result, and then the n-ths go round the other way - through the same lanes, each lead granting its senders its lane as it enters | O(P × bytes) for the lead's combine on a node, then the tree; a reduction's child waits for its parent's lane, once: up to a hop when the two enter together - nothing once the child's own node takes a hop - and up to a round trip, the grant down and the first piece back, when the parent enters after it. On the Pi cluster's four nodes, against 0.1.0's one tree (a fan-out of 4, two in flight), a 1 MiB broadcast took 1.43 to 1.47 times less time and a 1 MiB reduction 1.20 to 1.26, every size from 64 KiB up was faster, and no size slower beyond the run-to-run noise. Träff's: 2⌈log₂ n⌉ rounds, and every lead's link carries under two vectors where a tree's busiest carries k + 1. On the Pi cluster's four nodes, against those trees, a 1 MiB reduction took 1.32 to 1.41 times less time, 2 and 4 MiB 1.43 and 1.46 in one run each at a PE a node, every size from 16 KiB was faster - 1.04 to 1.58 times by OSU's benchmark and the project's probe, though `osoe-bench`'s loop of 64 KiB reductions back to back took as long as the trees' (2.26 ms against 2.24) - and every size to 8 KiB took the trees, unchanged; the cost model, not a measurement, puts the 1 MiB gain at about 1.7 times on seven nodes and more, where a tree has interior nodes, and on eight of Google Cloud's nodes, where the chooser took it, it ran 1 MiB 2.4 times faster than the tree it took elsewhere (`docs/fast-link-results.md`) |
| `fcollect` | Bruck's allgather among the node leads | ⌈log₂ n⌉ steps, each node receiving every other node's data once |
| `collect` | the lengths first, summed through the node leads' tree - each node's total, 8 bytes a node, where each node's PEs are one run of the team's ranks, as under a block distribution, and otherwise one vector of 8 bytes a PE, which a team's first such collect finds by a walk of its ranks; or, forced by `OSOE_COLLECTIVE_PLACES=scan` where the runs meet the nodes in their order or its reverse, a scan of the nodes' totals among the leads, each lead learning only its own node's place and the total; or, forced by `OSOE_COLLECTIVE_PLACES=exchange` on up to 64 nodes, each node's total put by its lead to every other; the default choosing among the three from the link's cost of each, the exchange on no more than 16 nodes - then the data as `fcollect`'s, under the scan each step carrying the byte ranges it sends | the data as `fcollect`'s; the lengths O(n) bytes up the tree and down it, 4 KB at 500 nodes - O(N) under a cyclic or plane distribution, 1 MB at 128,000 PEs; under the scan 8 bytes up and 16 down a message, and 40 bytes more a step of the data; under the exchange one hop and n - 1 messages of 16 bytes out of every lead; the walk O(M) lookups on each lead, once a team, and 8 bytes kept for each of its S nodes |
| prefix scans | a tree over the team's ranks, every member a place in it; a scan of at most `OSOE_SMALL_COLLECTIVE_BYTES` in one piece through slots its team keeps, a larger one pipelined through lanes as the large reductions are, in pieces sized to the scan, at most 16 KiB | O(log N) rounds; a large scan's child waits for its parent's lane, once: about a hop more when they enter together, up to a round trip when the parent enters after it - on the Pi cluster, where such a hop is about 120 µs with its handling, a 2 KiB scan measured 120 to 160 µs slower than through the per-team slots the lanes replaced, and 230 to 260 µs with its root or a member with children late |
| `alltoall`, `alltoalls` | every block put directly, then a barrier | O(N) a PE - inherent: every PE has a block for every other - and so P(N - P) blocks into a node, all applied by its one progress thread, a message each up to a chunk (`OSOE_AM_CHUNK`) and, for an `alltoalls` with a stride, one an element: 32.7 million messages a call at 500 nodes of 256 |
| a team | a number agreed by one reduction over the parent of every member's free numbers, the same on all of them, and a block from each member node's pool, given back at destroy | the reduction of `OSOE_MAX_TEAMS` / 64 + 1 words - about what one broadcast costs - and a second, of one word, when any PE of the parent has `SHMEM_THREAD_MULTIPLE`, then a sync of the parent: about a broadcast and a barrier, and a destroy a barrier of the team; O(P) a member, testing its node's PEs; on each node's lead, O(M log n) once to find the team's nodes from its members, a search of the rank map for each - O(M log M) more when they are not met in node order - and the S leads kept |
| waiting | a futex word per resource a connection has, shared by the node's PEs; one per thread for its replies and watches, per PE for its agent and its landing zone, per context for its counters | O(1) a waiter; room made in a connection's buffer wakes as many of the writers waiting for it as the room holds messages of a chunk, at least one, and a woken writer that took less than a chunk wakes one more (`OSOE_TURNOVER_WAKE`, Limits) |
| start-up | the node leads' rendezvous tree, then their mesh; then, where the large collectives' link is measured, the root's lead timing round trips and streams to its first children's, and one reduction of three words handing every PE its numbers | ⌈log₈ n⌉ levels, then 2(n - 1) connections a lead - a control wire and a data wire a peer - about half of them its own calls, over ⌈(n - 1)/2⌉ slots of 100 µs with at most eight in flight, and the rest its callers', answered meanwhile; then the measurement - about 27 ms of the Pi cluster's start as v0.2 step 2 timed it, before P38 added a round trip to each of the message cost's fifteen tries on three nodes or more, untimed there since (the `OSOE_LINK_*` row); its streams carry 1.9 MiB, so about 170 ms on a link of 100 Mbit/s - and the reduction's ⌈log_k n⌉ levels at the fan-out k |
| `SHMEM_THREAD_MULTIPLE` | threads issue on a shared context without a lock; every wait that completes its operations - a quiet, `shmem_pe_quiet`, a strided get's end, and the quiet a barrier or `shmem_clear_lock` makes - takes the context's, and one shares a full quiet that began and ended while it waited for it | O(1) an operation |

Where the design admits a limit of its own: a large broadcast, and a reduction
too small for Träff's rounds to pay - under 4 to 16 KiB by the node count on
the Pi cluster's link, by the cost model - go through a tree, whose busiest
lead sends k + 1 vectors where Träff's sends under two; on two nodes, where the
two send the same bytes, the cost model takes the tree at every size on the Pi
cluster's link; measured there (P4, 2026-10-29), the tree was the faster to
16 KiB, Träff's taking up to 1.7 times as long, and from 32 KiB the two's
medians came within 12% of each other, Träff's the faster from 48 to 128 KiB
and, at four PEs a node, to 2 MiB; Träff's takes 2⌈log₂ n⌉ rounds where a
tree takes 2⌈log_k n⌉ levels, each round a row of a lane, and a lane has the
ceiling's + 1 rows or the ⌈log₂ n⌉ the rounds need, whichever is more, so
Träff's runs on a team of up to 65,536 nodes at any `OSOE_COLLECTIVE_FANOUT` -
past that the reduce-scatter's words hold no more
rows, and a team takes the tree at every size - at the price, past 512 nodes at
the default of 8, of a longer lane, 128 KiB for each doubling of the nodes on
each of a PE's 17: 2.1 MiB a PE at 1,024 nodes, 6.4 at 4,096 (Memory, below);
and past 512 nodes there is no measurement of it, only the cost model, which
at the default window of 8 puts a 1 MiB reduction on the Pi cluster's link
2.05 to 2.47 times faster by Träff's than by the best tree from 513 to 4,096
nodes by its closed form, and by its event simulation, which runs the trees
faster than the closed form does there, 1.19 times at 513 nodes and 1.18 at
1,024 - the best tree 22.3 and 21.6 ms, Träff's 18.7 and 18.3 (at the window
of 4 it had until 2026-10-28, 3.2 to 4.3 times and 2.7 to 2.9); a call's shape
is chosen from one link's numbers, the root's lead's to its first children's,
so a job whose links differ is shaped for that one; every pair of
nodes connects twice at start-up, so the job holds n(n - 1) connections, 2(n - 1)
a node, and a pair's puts and gets go by one of the two, so a network that
limits or spreads traffic by the connection - a cloud's cap on one flow, a
fabric's hashing of flows to paths - gives a pair one flow's share; and, where
there are no progress helpers, one thread a node moves all of its internode
messages (Limits, below).

Within a node, each collective's phase is O(P): its PEs arrive on one cache
line, the release of a reduction or broadcast is one futex wake of every PE
waiting, and the node's lead combines its PEs' vectors one after another, so a
reduction costs the lead O(P × bytes). A barrier's release on a team that
spans nodes is a chain of wakes O(log P) deep, and on a team of one node one
wake of every PE. The one-node barrier is timed past 4 PEs a node: linear
to 32 on a four-core virtual machine, so oversubscribed past 4 - 11 µs at 2 PEs
and 42 at 32 when the machine was quiet (design §5.3), 13 to 18 and 105 to 116
under a review's load. The other phases are timed with 4 PEs a node on the Pi
cluster and with 8 and 16 on Google Cloud's eight nodes (OSU's collectives and
`osoe-bench`), and with 16 to 256 on two of them - a barrier of 512 PEs taking
672 µs (H12, `docs/fast-link-results.md`). All of these were timed with the
collectives' check (`OSOE_CHECK_COLLECTIVES`) off, as it was by default until
2026-10-28; H12 timed it on as well, and at 256 PEs a node the barrier of
`osoe-bench quick`, 200 trips after 200 of warm-up, took 975 µs with it on
against 672 off, every other line within 3.4% (Limits). That is mostly a cost
of the first barriers: a sync is checked by whichever PE of the node arrives
last, and the first time a PE checks one on a team it reads a page of each of
its P - 1 node-mates' regions, a minor fault each, on the barrier's path; in
the development VM, in H12's shape of two nodes of 256 PEs, the check cost a
barrier 3% from the 1,000th on (the tenth review's scale S2).

### Memory

Most of it is address space: an object in `/dev/shm` sized at start but sparse,
or memory allocated and not yet written, which costs a page only once written.
The `/dev/shm` capacity check counts the objects whole all the same.

| What | Where | Grows with | 4 × 4 | 64 × 64 | 500 × 256 |
|---|---|---|---|---|---|
| A PE's heap object: the symmetric heap, then the library's internal region | in `/dev/shm`, one a PE, mapped by every PE of the node | `SHMEM_SYMMETRIC_SIZE`, then 24.6 to 29.6 KiB a team number (`OSOE_MAX_TEAMS`) and a lane a thread place (`OSOE_MAX_THREADS` + 1), 1,153 KiB to 512 nodes and 128 KiB more for each doubling of the nodes past it | 281 MiB, the region 25.30 | 282 MiB, 26.17 | 282 MiB, 26.46 |
| What every PE maps of its node's `/dev/shm` | address space | P heap objects, the control segment and the connection area | 1.11 GiB | 17.7 GiB | 70.9 GiB |
| The control segment | in `/dev/shm`, one a node | about 1.0 MiB, about 27 KiB a PE and its landing zone for gets (`OSOE_GET_LANDING_BYTES`) - by default its node's 4 MiB split among the node's PEs, 1 MiB at 4 PEs a node and 256 KiB from 16, and 16 bytes an extent of 1 KiB besides - and a 128-byte context block for each context and each thread place (`OSOE_MAX_CONTEXTS`, `OSOE_MAX_THREADS` + 1) - 128 bytes a peer node, and 256 bytes an owner of the node's wires: the lead's agent and each progress helper | 5.2 MiB | 18.9 MiB | 72.8 MiB, 1.5 written at start |
| The connection area | in `/dev/shm`, one a node | 512 KiB of buffers and 1.25 KiB of state a peer node | 1.5 MiB | 31.6 MiB | 250 MiB, 0.6 written at start |
| The node lead's own | its private memory | about 0.8 MiB and 8.1 KB a peer node - the reply pool, and 4.6 KB of it the peer's control wire, 4 KiB its private send buffer - and 1 MiB of receive buffers as traffic comes; with progress helpers - two by default (`OSOE_PROGRESS_HELPERS=auto`) on three nodes or more where a node's PEs fit its CPUs - as much again of receive buffers each, 2 MiB for the two, and an io_uring ring and a thread's stack apiece; the figures here are for none | 1.8 MiB | 2.3 MiB | 5.7 MiB |
| Every PE's own, for the other nodes | its private memory | 24 bytes a peer node, and 21 a peer node for each context in use | 135 bytes | 2.8 KB | 22.5 KB |
| The job object: the rank map and each node's address | in `/dev/shm`, one a node | 136 bytes a node, and 24 a run of the rank map - n of them under a block distribution, P under a cyclic one, and under a plane or an arbitrary one as many as the ranks fall in, up to one a rank: 32,000, 768 KB a node, at 500 nodes of 256 under `plane=4` | 1.1 KiB | 10.4 KiB | 79 KiB |
| Sockets | the lead's | 2(n - 1) a node, a control wire and a data wire to each other node; n(n - 1) in the job | 6, 12 | 126, 4,032 | 998, 249,500 |

A team number is 24.6 KiB a PE at 4 nodes and 29.3 at 500: its counters and
small slots - one for each child position of the tree and one for releases, for
its reductions and broadcasts and again for its scans - its allgather's and
reduce-scatter's words, which grow with log₂ n, and collect's exchange of the
nodes' totals (P33), two counters' lines and 32 bytes a node up to 64 nodes:
2.25 KiB from 64 on. A lane is the ceiling's + 1 rows of the pipeline's slots,
1,153 KiB at the defaults; past 512 nodes at the default ceiling of 8 it has as
many rows as Träff's reduction has rounds, ⌈log₂ n⌉ (P3): 1,281 KiB at 1,024
nodes, 1,538 at 4,096 and 2,050 at 65,536, so that a PE's region comes to 28.7,
33.1 and 42.1 MiB where it was 26.5, 26.8 and 27.2 with the ceiling's rows
alone. A team block is a kilobyte of
the control segment on aarch64 (640 bytes on x86-64), about 760 bytes on the node's lead,
and about 500 bytes of address space on every PE.

**What one more of each setting costs** - a PE's own; every PE of a node maps
all P heap objects there, and the `/dev/shm` capacity check counts them so, P
times these:

| Setting | Each one more |
|---|---|
| `OSOE_MAX_TEAMS` | every PE 24.6 KiB of address space at 4 nodes to 29.6 KiB at a thousand - the allgather's and reduce-scatter's words grow with log₂ n, and collect's exchange's slots with n to 64 nodes - and 12 bytes of the control segment |
| `OSOE_TEAM_BLOCKS` | the node a kilobyte of `/dev/shm`, written at start; its lead about 760 bytes; every PE about 500 bytes of address space |
| `OSOE_MAX_THREADS` | every PE a lane of address space, 1,153 KiB to 512 nodes at the defaults, and 896 bytes of the control segment on aarch64, 768 on x86-64, 128 of them the context block its blocking gets of more than one piece go on |
| `OSOE_MAX_CONTEXTS` | every PE 128 bytes of the control segment and about 180 bytes, and each context in use 21 bytes a peer node |
| `OSOE_PROGRESS_HELPERS` | each node's lead a thread, an io_uring ring and `OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS` of receive buffers, 1 MiB at the defaults; the control segment 256 bytes; two are the default where `auto` applies |
| `OSOE_SMALL_COLLECTIVE_BYTES` | a team number eighteen bytes for each byte of the value, on about 11.3 KiB: 13.5 KiB at 128, 29.3 KiB at the default 1 KiB, 299 KiB at 16 KiB, a PE (at 500 nodes) |
| `OSOE_COLLECTIVE_FANOUT` | a team number 2.5 KiB and, to 512 nodes at the default - past it a lane has the rounds' rows already - a lane 128 KiB, a PE; the tree is at most ⌈log_k n⌉ levels deep - 3 at 500 nodes at the default 8, 5 at 4, 2 at 32 |
| `OSOE_COLLECTIVE_PIPELINE` | a lane 144 KiB, 2.4 MiB a PE across its 17 |
| `OSOE_GET_LANDING_BYTES` | the control segment about that much a PE, and 16 bytes a KiB of it; by default 4 MiB a node, from 1 MiB a PE to 256 KiB |
| `OSOE_AM_CHUNK`, `OSOE_AM_BUFFER_BUDGET` | the connection area eight chunks a peer node at the default buffer, four at its floor; the budget then holds the default to budget / (8 × chunk) + 1 nodes, 2,049 at the defaults, and caps the job at budget / (4 × chunk) + 1, 4,097 - or, its data wires on demand, a node at budget / (4 × chunk) data wires, 4,096, whatever the job's nodes |
| `SHMEM_SYMMETRIC_SIZE` | the address space every PE maps P times |

### What does not scale, and the limits

- **One progress thread a node**, where there are no helpers - on a job of one
  or two nodes, and by default where a node runs more PEs than it has CPUs -
  moves every internode message of its node: on the Pi cluster's four PEs a
  node it keeps up with gigabit's bandwidth, and its message rate depends on
  where it runs - a pipelined pattern ran at a sixth of its rate under Slurm's
  default binding, a core a task, against `--cpu-bind=none`
  (`docs/pi-cluster-results.md`); at 256 PEs a node its message rate has not
  been measured, and its streams there moved a third of the link on two Google
  Cloud nodes, at twice the CPU a GiB of 16 to 64 PEs a node (H12). Progress
  helpers (`OSOE_PROGRESS_HELPERS`; two by default on a job of three nodes or
  more where a node's PEs fit its CPUs) spread a node's data wires over more
  threads, a share of the other nodes each; on four nodes of the Pi cluster
  they cost broadcasts and alltoalls more than they bought, and on eight of
  Google Cloud's, unbound, they took every stream to the provider's egress
  ceiling, and under Slurm's binding a fifth faster with a core a PE and little
  with one CPU a PE (Limits).
- **`alltoall`** sends P(N - P) blocks into a node, a message each up to a
  chunk and, for an `alltoalls` with a stride, one an element, all applied by
  that thread.
- **`collect`'s lengths** are a vector of N eight-byte words summed through the
  tree where a node's PEs are not one run of the team's ranks - under a cyclic
  or plane distribution - 1 MB a call at 128,000 PEs; under a block
  distribution, n words, each node's total, or under the scan
  (`OSOE_COLLECTIVE_PLACES=scan`) a word a node up the tree and two down.
- **Prefix scans** run over a tree of the team's ranks, every member a place in
  it, not over its nodes: O(log N) rounds, a message across nodes wherever a
  rank's neighbour in the tree is on another node.
- **The deprecated active-set collectives** run PE by PE: a binomial tree over
  the set's ranks, log₂ of its size deep, its edges across nodes wherever its
  PEs are.
- **The mesh**: every pair of nodes connects twice at start-up, a control wire
  and a data wire - or, with `OSOE_MESH_DATA_WIRES=on-demand`, its control wire
  at start-up and its data wire at the first traffic between the two - by a
  schedule (`docs/design/mesh-schedule-proposal.md`): each
  lead calls the leads one to (n - 1)/2 places after it round the circle - of
  an even count's opposite pairs, the lower - and answers the rest, one place a
  slot of 100 µs (`OSOE_MESH_SLOT`) in an order every lead draws alike from the
  job's ids, at most eight calls in flight, its callers answered throughout.
  Before it a lead called the higher leads one after another once every lower
  one had called it, a chain. Over a Pi-like link, 100 µs each way, with a CPU
  a lead, the model (`tools/modelcheck/mesh_timing.py`) connects 64 nodes in
  4.6 ms where the chain took 64 ms, 1,024 in 71 ms where it took 1.0 s, and
  4,097 in 0.26 s where it took 4.1 s - with the leads arriving as the
  rendezvous's tables reach them, 6.3 ms, 0.11 s and 0.45 s. No system large
  enough has measured it, which the author waived; tier 3 has, its logical
  nodes sharing one virtual machine's four CPUs: 64 nodes in 82 to 105 ms where
  the chain took 156 to 173, and 16 in 5 ms where it took 21 to 25; over
  loopback, where those CPUs are the limit, 254 nodes take the same 0.65 to
  0.75 s either way. On one subnet each node also needs an entry in the
  kernel's neighbour table, its record of a peer's Ethernet address, for each
  of its n - 1 peers, and Linux allows 1,024 by default: for more than about a
  thousand nodes on one subnet, an administrator raises
  `net.ipv4.neigh.default.gc_thresh3` above the node count, and `gc_thresh1`
  and `gc_thresh2` with it. Logical nodes in network namespaces of one machine,
  as tier 3 makes them, share that machine's table, n × (n - 1) entries -
  more than 1,024 past 32 nodes, where `tier3.sh` refuses by name.
- **`/dev/shm`**: each PE makes a heap object, which every PE of its node maps,
  so the capacity check asks for P × (`SHMEM_SYMMETRIC_SIZE` + the program's
  statics + the internal region, 25.3 MiB at 4 nodes to 26.5 at 500) and the
  control segment - 70.7 GiB a node of 256 PEs on 500 nodes at the default
  heap, though pages cost only as written - and the connection area checks its
  own, 250 MiB at 500 nodes; lower the heap, or set
  `OSOE_SHM_CAPACITY_CHECK=0`. The landing zones are a little over
  `OSOE_GET_LANDING_BYTES` × P of the segment, 16 bytes of record a KiB of zone
  and a quarter of a KiB of header besides: 65 MiB at 256 PEs, and by default,
  each node's 4 MiB split among its PEs, 4.1 MiB on a node of 4 to 16 PEs.
- **A node's team blocks** run out first at many PEs a node: three whole-job
  `shmem_team_split_2d` grids are as many as the default pool holds at 256 PEs
  a node, and a fourth fails (Limits, "Teams").
- **The ceilings**: 4,097 nodes at the default connection budget (2,049 before
  the buffers halve) - with the data wires on demand, 4,096 data wires a node
  instead, and 12,288 nodes at no progress helpers or 16,384 with them, the
  ceiling of a lead's io_uring; 65,535 PEs a node; `OSOE_MAX_TEAMS` 1,024,
  `OSOE_TEAM_BLOCKS` 65,536, `OSOE_MAX_THREADS` 1,023, a fan-out of 32 and 8
  pieces in flight, at most. The next heading gives every one, with how far
  each has run.

### Ceilings, and how far each is tested

Each bound a job can meet, its value, what happens past it, and the largest
value any run has reached, and where. What has run is the measure of every row:
in the registered suite, jobs of up to 70 logical nodes, and 70 PEs on one node;
on one machine's logical nodes, by the records, 254 nodes under `osoe-launch`
and 128 under tier 3; and across separate machines, eight Google Cloud nodes -
256 PEs at 32 a node, and 512 at 256 on each of two, streams of puts and five
collectives alone - and the Pi cluster's four nodes of four PEs. So most of the
ceilings below are far past any job that has run: where osoe sets one, its
arithmetic is tested, and a job at it is not. [`docs/ceilings.md`](docs/ceilings.md)
has every ceiling, with where in the source each is set, the record of each
run, the layout's field widths that no run approaches and the widest gaps; each
setting's own range is its row under [Environment variables](#environment-variables).

| Ceiling | Value | Past it | Largest run, and where |
|---|---|---|---|
| Nodes a job | 4,097 at the default `OSOE_AM_BUFFER_BUDGET`, 1 GiB; from 2,050 the buffers to each peer are halved to their floor, twice `OSOE_AM_CHUNK` | from 2,050 each node's lead says the buffers were reduced; from 4,098 the start ends, naming the budget | 254 logical nodes on one machine; eight Google Cloud nodes; the arithmetic at 2,049 to 4,098 |
| Data wires a node, under `OSOE_MESH_DATA_WIRES=on-demand` | 4,096 at the default budget, whatever the job's nodes | a data wire past them ends the job, naming the node and both settings | four logical nodes at two data wires a node |
| Nodes, a lead's io_uring | 8,192 at no progress helpers, which only a larger budget reaches; 12,288 with the data wires on demand; 16,384 with helpers | refused at start by name | the arithmetic at each; 254 logical nodes |
| Nodes, under `OSOE_WIRE_ZERO_COPY=on` | 3,274 at no helpers and the default receive buffers, by io_uring's completion queue | refused at start by name | 64 logical nodes; eight Google Cloud nodes |
| Nodes on one subnet | about 1,025: the kernel's neighbour table, an entry a peer, holds 1,024 by default (`net.ipv4.neigh.default.gc_thresh3`); logical nodes of one machine share one, n(n - 1) entries, more than 1,024 past 32 nodes | the kernel drops what it cannot address, calls time out, and nodes count each other vanished: osoe does not check it; `tier3.sh` does | 128 tier-3 nodes with the table raised, 32 at the default; eight Google Cloud nodes |
| Callers waiting at a lead | a listen backlog of the job's nodes, up to 4,096, cut to `net.core.somaxconn`; 16 at a time through the hello | a caller waits in TCP's retries, and osoe dials again after a second; on one kernel at 128 tier-3 nodes, its per-CPU queue of arriving packets (`net.core.netdev_max_backlog`, 1,000) dropped handshakes, a lead a second late in 12 runs of 30 | 254 logical nodes |
| Rendezvous ports | 32 candidates of `OSOE_PORT_RANGE` a node, drawn from the job's ids | the start ends, naming the range | 254 logical nodes on one host; the refusal untested |
| PEs a node | 65,535 | the start ends, naming the rank list | 256 on each of two Google Cloud nodes, streams of puts and five collectives; 64 with gets and the mailbox; 70 on one node in the suite |
| PEs a node, by the launcher's one string | a node's ranks come as one environment string, `SLURM_GTIDS`, and Linux holds one to 32 pages, 131,072 bytes on 4 KiB pages: about 21,000 PEs of five-digit ranks | the launcher cannot start the process ("Argument list too long") | not run |
| PEs a node, by the kernel's map count | each PE maps each node-mate's heap and statics, two maps a node-mate, against `vm.max_map_count`, 65,530 by the kernel's default: about 32,000 node-mates; osoe does not check it beforehand | the start ends, naming the node-mate's heap it could not map | not run |
| PEs a job | none of its own beside the nodes' and a node's; designed for 500 nodes of 256, 128,000 PEs | - | 512 |
| Open files | a lead's two sockets for each other node and a descriptor for each node-mate, beside the program's: a hard limit of 1,024 runs out at about 385 nodes of 256 PEs | the start ends, naming the limit | 254 logical nodes; the refusal at a limit of 32 |
| Locked memory | io_uring's rings on Linux 6.14 and later, at most 2.6 MiB a lead at 4,097 nodes on a Pi's pages; the zero-copy send's loans | a ring refused ends the start, naming the limit; a send refused goes by copy | 254 logical nodes on a Pi; 64 at 8 MiB with the zero-copy send |
| `/dev/shm` | P × (the heap, the statics and about 26 MiB) and the control segment: 70.7 GiB a node of 256 PEs on 500 nodes at the default heap | refused at `shmem_init` by name, or a warning with `OSOE_SHM_CAPACITY_CHECK=0` | 256 PEs a node on a heap of 16 MiB |
| A PE's heap, `SHMEM_SYMMETRIC_SIZE` | 4 TiB | refused by name | 1,500 MiB on the Pi |
| `shmem_align`'s alignment | 4 GiB, which every heap base the library tries is a multiple of | returns NULL on every PE | 4,096 |
| Teams a PE is in, `OSOE_MAX_TEAMS` | 1,024; 256 by default | a split returns nonzero, naming it | 1,021 at once on four PEs; 12 on the Pi |
| Team blocks a node holds, `OSOE_TEAM_BLOCKS` | 65,536; 1,024 by default, which three whole-job `shmem_team_split_2d` grids fill at 256 PEs a node | a split returns nonzero, naming it and the node | 8,192 at 256 PEs on each of two logical nodes |
| Contexts, `OSOE_MAX_CONTEXTS` | 4,096; 64 by default | `shmem_ctx_create` returns nonzero, naming it | 4,096 on two logical nodes |
| Live threads that have called the library, `OSOE_MAX_THREADS` | 1,023; 16 by default | the job ends, naming it | 21 |
| CPUs `OSOE_AGENT_CPUS=node` can place an agent on | those numbered below 1,024, as glibc's `cpu_set_t` holds them | a CPU from 1,024 on is not recorded, and nothing says so | not run |
| Progress helpers, `OSOE_PROGRESS_HELPERS` | 64 | refused by name | 64 on two and four logical nodes; 4 on eight Google Cloud nodes |
| A lead's reply pool | written whole at start, 48 bytes a descriptor: one for each credit (`OSOE_AM_CREDITS`) of each peer's data wire and one more a peer, and (`OSOE_COLLECTIVE_FANOUT` + 1) × `OSOE_TEAM_BLOCKS` - 432 KiB of those at the defaults; at 65,536 credits and 4,096 peers, about 12 GiB | a pool that runs dry ends the job, naming both nodes: only nodes whose `OSOE_AM_CREDITS` differ, or a defect, can empty it | 64 credits; 1 and 2 in the suite |
| A wire owner's receive pool | `OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS`, 1 MiB at the defaults, however many peers send to it | not an error: the kernel ends a receive with `ENOBUFS` and the owner arms it again, counted in `transport.am.receive_enobufs` with no notice, so the counters alone show it | 256 buffers of 16 KiB, and 64 of 64 KiB, on eight Google Cloud nodes; a fan-in of seven nodes into one |
| A tree's fan-out, `OSOE_COLLECTIVE_FANOUT` | 32 | refused by name | 32 on three logical nodes; 8 on eight Google Cloud nodes |
| Träff's reduction | teams of up to 65,536 nodes | the tree | 70 logical nodes; eight Google Cloud nodes; past 512 nodes only the cost model |
| `collect`'s exchange of the nodes' totals | room for 64 nodes; 16 by default (`OSOE_COLLECTIVE_PLACES`) | the tree or the scan; a forced exchange falls back | 17 logical nodes; eight Google Cloud nodes |
| `OSOE_KEEPALIVE` | IDLE and INTERVAL 1 to 32,767 s, COUNT 1 to 127 | refused by name | the default |
| A host list | one bracketed range a name: `node[1-8]`, not `rack[1-4]n[1-8]` | `shmem_init` ends, naming the list | the parser's tests |
| `osoe-launch` | 1,024 processes; 254 logical nodes without a host list | refused by name | 254 logical nodes; 512 processes |

The widest gaps between a ceiling and its largest run are a PE's heap, about
2,800 times what has run; a connection's credits, 1,024 times the default that
every run used; Träff's teams, 936 times the 70 logical nodes it has run on;
PEs a node, 256 times - though the launcher's string stops them at about
21,000; and, against separate machines alone, the 4,097 nodes, 512 times the
eight that have run. The off-network cluster's brief
([`docs/prompts/off-network-cluster-brief.md`](docs/prompts/off-network-cluster-brief.md))
asks what only a cluster of hundreds of nodes can measure.

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
  parent's rank 0 says why, naming the variable. Each number costs every PE
  about 25 KiB of address space at the default `OSOE_SMALL_COLLECTIVE_BYTES` -
  its counters and the slots of its small collectives and scans, the
  allgather's and reduce-scatter's words, which grow with the node count's
  logarithm, and collect's exchange's slots, which grow with it to 64 nodes -
  to about 29.6 KiB at a thousand nodes, which the `/dev/shm` check counts.
- **A node holds at most `OSOE_TEAM_BLOCKS` teams at once** (1,024 by default),
  the three predefined among them, a team of members on several nodes counting
  once on each, and a team of one PE not at all: it claims no block, holding
  nothing of the node's. A split that would make one
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
- **A thread must not be cancelled inside a call, nor leave one by
  `longjmp`.** The specification defines neither. A thread cancelled in a large
  blocking get leaves its pieces in flight, and they land after it is gone -
  into its buffer, freed or an unwound stack - or stay in the zone, where the
  next thread in its place can wait for bytes no one copies (the ninth review's
  C9-3); and the deadlock detector counts the thread as waiting for ever, so a
  program that does either must turn it off, `OSOE_DEADLOCK_SECONDS=0`
  (`base/sweep.h`).

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
  nodes, at no helpers, a node's io_uring ring cannot hold its wires - the
  kernel gives a ring 32,768 submissions, two a wire and one - and past 16,384
  with helpers, which take the data wires off the lead's ring; such a job is
  refused at start-up, naming both counts and `OSOE_PROGRESS_HELPERS`.
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
  (`OSOE_GET_LANDING_BYTES`, in extents of 1 KiB; by default its node's 4 MiB
  split among the node's PEs, no less than 256 KiB nor more than 1 MiB a PE): a
  get takes whole extents, at least one, so 256 small gets in flight fill a zone
  of 256 KiB as 256 KiB of data does, and past it a new get waits for earlier
  ones to land - backpressure, never an error. Where one node owes every piece
  the zone holds unanswered, the wait names it, and its PE where one PE does -
  in `osoe-stat`, and in the line `OSOE_NO_PROGRESS_TIMEOUT` ends it with.
- **A large get from another node is held by its PE's landing zone.** A get's
  pieces of up to 64 KiB - seventeen to a mebibyte - go out together, a
  blocking get's as a nonblocking get's, but no more of them than the landing
  zone holds (`OSOE_GET_LANDING_BYTES`): four at 256 KiB, sixteen at 1 MiB.
  The zone is the PE's, shared by its threads and contexts, so while one large
  get fills it another thread's get of any size waits for a piece to land; and
  the pieces in flight toward a node are bounded by its connection's reply
  credits too (`OSOE_AM_CREDITS`, 64, shared by the node's PEs), from sixteen
  PEs of a node getting at once. With a zone of 256 KiB, the default then, on
  the Pi cluster's gigabit a blocking get of 64 KiB or more took 1.00 to 1.06
  times a put and quiet of the same size, and a nonblocking one 0.98 to 1.06;
  over the development VM's loopback, where a round trip's time carries about
  256 KiB, about 2.7 times at a mebibyte and 3.3 at 4 MiB. On Google Cloud's
  link of 23 Gbit/s a blocking get took 1.6 times a put and quiet at a
  mebibyte and 1.8 to 1.95 times from 4 MiB with that zone, and what a put did
  from a zone of 1 MiB, while eight PEs on each node getting from the other
  nodes ran fastest with 256 KiB, 13,601 MiB/s a job against 8,786 with 16 MiB
  (`docs/fast-link-results.md`). So by default a node's 4 MiB is split among
  its PEs, no less than 256 KiB nor more than 1 MiB a PE - 1 MiB to each of one
  to four PEs a node, 512 KiB to each of eight, 256 KiB from sixteen - as the
  author decided on those numbers (P32; `docs/decisions/2026-10-12.md`;
  `docs/decisions/2026-10-10-blocking-gets.md`;
  `docs/design/blocking-get-proposal.md`, (d)); on the cluster (H28, 2026-10-28)
  it was the best or within 1% of the best at 1, 4, 8 and 16 PEs a node, where a
  fixed 256 KiB cost one PE a node 37% and a fixed 1 MiB cost sixteen 25%; each
  PE's start-up line under `SHMEM_DEBUG` names its zone. A
  thread whose place's
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
  `shmem_init` raises the soft limit to it, and a node that runs out at start
  ends naming the limit it had and that it is the hard limit to raise. On Linux 6.14 and later io_uring
  also charges a node's rings to its user's locked memory, every process of the
  user on the host together, against each one's `ulimit -l`, whose soft limit
  `shmem_init` raises to the hard one too, zero copy or not: a ring the hard
  limit refuses ends the job at start-up, naming it. Only one machine's logical
  nodes come near it - one node's rings took at most 2.6 MiB at the 4,097-node
  ceiling, on a Pi's 16 KiB pages at up to three helpers - and [`osoe-launch`](#running-a-program-on-one-machine-with-osoe-launch)
  gives a Pi's numbers.
- **A program linked statically is refused**: `oshcc -static` says so - and
  `--static` and `-static-pie` - since the C library's own state would then be
  gathered with the program's symmetric statics.
- **Tested on aarch64, and on x86-64 since 2026-10-12.** osoe has been developed
  and tested on aarch64: the Raspberry Pi 5 cluster and an aarch64 virtual
  machine. It has run natively on x86-64 in three sessions, on eight Google
  Cloud nodes ([Requirements](#requirements) says which runs, [the
  results](docs/fast-link-results.md) has them): the floor's timing pins, set on
  aarch64, held, and messages crossed between separate machines - the floor's
  round trips, a bare stream, gets, a fan-in, OSU's benchmarks and the sessions'
  sections, up to 512 PEs. The suite, which checks results, ran on one node, a
  job's logical nodes talking over loopback, so it has not run across separate
  x86-64 machines; the stress tester has, its `mix` for 300 s at 32 PEs on all
  eight without a failure, and Sandia OpenSHMEM's suite, 159 of the 163 programs
  of the cluster kit's sweep, the same four short as everywhere, at 2 PEs a
  node on eight (2026-10-28); 14 of the 41 models
  (Python, so the same result on any machine) have not run natively at all.
  Before that,
  on x86-64 the tree tagged 0.1.0 built without warnings and passed every test
  that can run under Rosetta, an x86-64 user space on an aarch64 kernel - 369 of
  its 718, with a shim standing in for the futex2 calls Rosetta refuses; the
  emulator refused the rest io_uring or seccomp - and v0.2 step 1's full build
  passed its one-node tests but four, each failure the emulator's or the cross
  build's. 0.2.0's tree cross-builds for x86-64 without a warning, but its full
  build's one-node tests stalled Rosetta's translation daemon in each of three
  runs (2026-10-30), and so have not run whole there. Rosetta refuses io_uring, so no x86-64 process had sent a message to
  another node: the network path, and the timing pins that were set on aarch64,
  were untested there, and no x86-64 kernel had run any of it. What x86-64's
  memory order would do to the protocols across nodes was
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
  shim, since Rosetta passes the classic futex through: 343 of its 349 tests at
  v0.2 step 1, and 578 of its 585 on 0.2.0's tree,
  each failure the emulator's or the cross build's. Natively on x86-64 it has
  run on Debian 13's kernel alone.
- **Every node and every PE of a job must run the same osoe.** The nodes say
  hello with the library's version, and a node of another is not taken; the
  PEs of a node meet in its control segment, laid out by the version that made
  it. A job that mixes versions - a site upgrading its nodes one at a time, a
  rank started with another `LD_LIBRARY_PATH` - waits until its time limit, its
  notices saying for whom, rather than ending by name: 0.1.0's PEs can do no
  better. From layout 37 on, the control segment's rendezvous words keep their
  place, so a later osoe's PEs meeting this one's are refused by the layout.
- **Every node of a job must have the same architecture**, and the rendezvous
  refuses a job that mixes them, naming both. Beside the job's size, the heap's
  size, the build's table of message handlers, the collectives' fan-out, team
  count, pipeline depth, small collectives' size and shape, the way to
  `collect`'s places (`OSOE_COLLECTIVE_PLACES`) and the link set by hand, it
  compares each node's byte order, `long`'s size and `long double`'s size and
  format - which x86-64 (the x87's 80-bit extended format) and aarch64
  (IEEE binary128) do not share, so that their `long double` values would
  otherwise be wrong with no message, in reductions and in puts and gets of
  them. Nobody has run such a job; the refusal is tested by a node told to
  claim the other architecture (`OSOE_TEST_ARCHITECTURE`).
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
  for the users who run jobs, prevents it, as the development VM showed (Debian
  13, systemd 257): a job of four PEs, one of them 25 s late in calling
  `shmem_init`, was started just after its user's only login ended. With the
  defaults the names were gone ten seconds after the login ended, and the
  launcher's timeout ended the job still waiting for that PE; with lingering
  enabled, and with `RemoveIPC=no`, it started and finished. A job the login
  itself started in the background was not caught: logind keeps that login's
  session until the job's processes are gone, with `KillUserProcesses` off, as
  it is on Debian 13. The record is
  `docs/design/evidence/v0.2-step4/2026-10-21-b10/`.

**Failures**

- **A PE or a node that dies ends the whole job**, with a message naming it; a
  node whose PEs do not share a PID namespace, as separate containers can make
  it, is refused as it starts (Building). There is no fault tolerance.
- **A process that leaves without calling `shmem_finalize` ends the job.** The
  specification has a program end its use of the library with `shmem_finalize`
  or `shmem_global_exit`, and a PE that simply returned from `main` would leave
  the others waiting for it at their next collective. So its exit ends the whole
  job: it says which PE left without finalizing, and every PE ends with status
  1, whatever status the process was returning. A program started with
  `start_pes` is finalized at exit, as the specification says, and is not
  affected. Older programs that never finalized, which some implementations let
  pass, fail here.
- **`shmem_global_exit` runs `atexit` handlers only on the callers it sees in
  time.** The specification has it flush I/O and release resources as a
  normal program termination does (§9.1.6), and such a termination runs a
  program's handlers. A caller whose call the library saw before the job's end
  reached its PE - the one whose call carries the job among them - leaves by
  `exit`, its handlers run: on the library's own thread, not the one that
  called, with signals blocked, and for at most 10 s, after which the library
  ends it. Every other PE flushes its streams and leaves by `_exit`, running
  none: a PE that did not call, which may still be computing, and a caller whose
  call the job's end outran - another node's goodbye, or another caller's
  declaration on its own node. When every PE calls at once, as each does that
  detects the same failure, that can be many of them, so a program whose
  handlers must run - a trace flushed, a file closed - does that work before it
  calls ("How a job ends").
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
- **A collective that the PEs of one node call differently is named by
  default.** The leads of the nodes compare a reduction's, a broadcast's or a
  collect's epoch, kind and size - a reduction's operation and element, a
  broadcast's root node - where they meet, and a disagreement between nodes
  ends the job naming it ("PEs called different collectives, or the same one
  with different arguments"). The PEs of one node are compared too, unless
  `OSOE_CHECK_COLLECTIVES=0`: each PE says its call - its kind, operation,
  element, bytes, root or strides - where the node's PEs meet, and the one that
  meets them all, a data collective's lead or the last to reach a barrier, ends
  the job naming two PEs and their calls where they differ, before any result
  is given out; an `alltoall` is compared at the barrier that ends it, after
  its puts, and a scan over a team spanning nodes, which has no meeting within
  a node, not at all. With the check off, one that broadcasts where the others
  reduce, or passes another count, root or `fcollect` size, gets or gives wrong
  data, and nothing says so - the job can end with status 0. A PE at 0 beside
  PEs at the default is named by the one that compares. On by default since
  2026-10-28 (P39), after it was timed at 16 to 256 PEs a node (P24, H12,
  2026-10-28): it cost nothing one round could see from 16 to 64 and, at 256,
  nothing but the barrier's 672 µs to 975 in `osoe-bench`'s short run - mostly
  a start-up cost: the first time a PE checks a sync on a team, as the last of
  its node to arrive, it reads a page of each node-mate's region, P - 1 minor
  faults on the barrier's path, and after a thousand barriers the check cost a
  barrier 3% in the development VM, in H12's shape (the tenth review's scale
  S2). The specification leaves a collective that its PEs do not call alike
  undefined. Every node's lead decides from the count it is given
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
  all when it is small or its team lies on one node (but by default, with
  `OSOE_CHECK_COLLECTIVES` on, a one-node team's is compared), where PEs
  given other counts get wrong results with nothing said; given counts on
  either side of the line, they end the job by name or wait until the
  deadlock detector ends it.
- **A team split is compared, by contrast**: every parent PE's start, stride
  and size - or `xrange` - go into the split's reduction, and PEs that passed
  others end the job by name, naming what each passed.
- **So is a symmetric-heap call**: every `shmem_malloc`, `shmem_calloc`,
  `shmem_align`, `shmem_malloc_with_hints`, `shmem_realloc` and `shmem_free`
  that does something first ANDs a word of its arguments, and the word's
  inverse, over every PE - one small reduction a call - and PEs that passed
  others end the job by name, naming what each passed (P23), where until then
  the next object started at another place on the PE that differed and puts to
  it landed in the wrong place with nothing said. The specification leaves such
  a call undefined. A PE that does nothing - a size of 0, a null pointer to
  free - where the others do something returns at once, and the others wait in
  the check until the deadlock detector ends the job.

**Transport and API**

- **TCP only**; no RDMA backend exists (below).
- **A floating-point reduction's result depends on the collectives' shape**,
  which at the default the link measured at start-up chooses for each size
  (`OSOE_COLLECTIVE_SHAPE`): two runs whose links measure either side of a
  crossover may differ in the last bits, and more where a sum overflows on the
  way, a NaN is among the inputs or zeros of both signs meet. Over 4,096 doubles
  on five nodes, each shape forced by a link set by hand, a sum that overflows
  on the way was infinite in 2,458 elements at 0.1.0's fan-out and pipeline,
  1,639 under a tree the chooser takes and 819 under Träff's, and a maximum
  with a NaN among its inputs fell below the largest number in 819 under that
  tree, 656 under Träff's and none at 0.1.0's fan-out and pipeline - since a
  maximum is `x > y ? x : y`, a NaN's place in the order decides (the ninth
  review's conformance C1). The specification allows it.
  `OSOE_COLLECTIVE_SHAPE=fixed`, or the link's three numbers set
  (`OSOE_LINK_*`), makes a run repeat its results. Under `fixed` that is the
  same bits from every run of this version for the same team, node map and
  settings, at any number of progress helpers - a node combines its members
  by local id and a parent its children by position, whichever thread carries
  their bytes - and whatever else the machine runs: separate launches of
  reductions like these, at no helpers, two and three, idle and beside a busy
  loop a core, give one hash (`shmem.fixed-shape.*`). Nothing is promised
  across versions, which stay free to change the leads' tree; `fixed` with
  `OSOE_COLLECTIVE_FANOUT=4 OSOE_COLLECTIVE_PIPELINE=2` takes 0.1.0's fan-out
  and pipeline.
- **Without progress helpers one thread a node moves every message between
  nodes** - on a job of one or two nodes, and by default where a node runs more
  PEs than it has CPUs (below). The progress thread of a node's first PE seals,
  sends, receives and applies the traffic of all the node's PEs, so a node's
  message rate and its bandwidth to other nodes are one thread's, whatever its
  PE count, and each PE's share of the rate falls as the PEs on a node rise.
  `OSOE_PROGRESS_HELPERS` gives the
  lead's process up to 64 more threads, each holding the data wires to a share
  of the other nodes - the peer that is p-th in node order, counting from 0 and
  the node itself left out, is helper 1 + (p mod H)'s - while the lead's agent keeps the control
  wires, so a barrier's messages are one thread's whatever the helpers do. A
  peer's operations stay on one wire and one thread, applied in order, so a
  stream between two nodes is still one thread's: what helpers add is the
  traffic to several peers at once. On the Pi cluster's four nodes of four PEs
  two helpers made a 64 KiB broadcast and a 4 KiB alltoall 6 to 35% slower
  under Slurm's default binding, where a lead's threads share its core, and 2
  to 9% slower with `--cpu-bind=none`; a 64 KiB reduction took 15 to 37% less
  time bound and about the same unbound
  (`docs/reviews/archive/v0.2-step4-scale-and-performance.md`, S4). On eight
  nodes of Google Cloud's 23 Gbit/s link, unbound, they pay: two helpers
  raised a receiving node's rate by half or more in a fan-in of seven nodes'
  PEs into one PE, and in the sweep at 0, 1, 2 and 4 helpers took every stream
  from 67 to 80% of the provider's egress ceiling to 97 to 99% at 8, 16 and 32
  PEs a node and the alltoalls a quarter faster, slowing no collective - but
  with more PEs on a node than vCPUs they halved the mailbox's small messages,
  and four already took a fifth off it at one PE a vCPU
  (`docs/fast-link-results.md`). So the author decided on two by default on a
  job of three nodes or more where a node runs no more PEs than it has CPUs
  (P36), once a site's binding confirmed it (H26). Measured so on 2026-10-28,
  with Slurm binding each PE to a core two helpers took the streams a fifth
  faster and halved the mailbox, and binding each to one CPU, which they then
  share, they bought the streams 1 to 5% and cost the 4 KiB alltoall 15%, the
  mailbox a tenth of its unbound rate either way - little either way at one CPU
  a PE. That rule, `OSOE_PROGRESS_HELPERS=auto`, has been the default since the
  author's word of 2026-10-28 (P36); `OSOE_PROGRESS_HELPERS=0` keeps one thread
  a node, for a job that lives by its message rate. Under `osoe-launch` the
  rule counts the machine's PEs, all its logical nodes', against its CPUs
  (`OSOE_MACHINE_PES`): a few logical nodes whose PEs fit take two helpers
  each, and sixty-four on four CPUs take none. On the Pi cluster's gigabit one
  thread already runs a stream at the link's rate, and the four-core virtual
  machine the suite runs on has no core to spare for a helper; the rule counts
  PEs against CPUs, not spare cores, so the Pi's four nodes of four PEs take
  two by default, where the measurement above found them a loss for the
  broadcast and the alltoall, and `OSOE_PROGRESS_HELPERS=0` there keeps one
  thread. They are built and tested - the suite runs at 0, 2 and 3 helpers a
  node and with the setting unset (How osoe is tested) - and cost each node's
  lead a thread, an io_uring ring and a megabyte of receive buffers apiece; by
  default there are two where the rule says so. "Put streams run at line rate"
  is a measurement at 1 Gbit/s - 111 to 112 MiB/s on the Pi cluster, a bare
  stream's own rate - and says nothing above it. On Google Cloud's link, the
  one faster link measured, the library's stream ran at 0.999 of a bare
  stream's rate, and a bare stream each way already reached the 2,718 MiB/s
  that Google allows a `c4-standard-16` (`docs/fast-link-results.md`). Over
  loopback between two nodes of a four-core virtual machine
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
  a copy, took the stream to three quarters; on Google Cloud's link, between two
  nodes, a put filling the room raised the stream 10 to 11% and halved its
  writers' sleeps, and parsing in place 2 to 5% more (H6, 2026-10-28). That
  thread still limits one
  stream: its time is the kernel's copy, the TCP stack and the one copy into the
  heap. The cost per message of
  one thread serving 256 PEs has not been measured on any machine: the Pi
  cluster's nodes run four PEs each, and on two of Google Cloud's the mailbox
  ran at 16 to 64 PEs a node - 697,000 messages a second at 16 and 32, 372,000
  at 64 - and at 256 only the streams, a third of the link at twice the CPU a
  GiB (H12); the stress tester's sixteen-PE mailbox, a pipelined point-to-point
  pattern, with `--cpu-bind=none`, ran at 256,000 messages a second over a
  minute. And until 2026-10-29 it grew with the PEs of a node waiting to send
  to one other node at once: each reopening of that connection's buffer woke
  all of them, and all but the few its room held slept again - on two Google
  Cloud nodes every writer slept once a reopening, the PEs' sleeps a 64 KiB
  moved a quarter of the PEs a node from 16 to 256, and at 256 the progress
  threads' CPU a GiB was twice that at 16 to 64 (H12,
  `docs/fast-link-results.md`). A reopening now wakes as many as its room holds
  messages of a chunk, four at the defaults (`OSOE_TURNOVER_WAKE=room`): in the
  virtual machine, from 4 to 64 writers putting 64-byte to 1 MiB blocks to one
  other node, it moved 0.91 to 1.18 times what waking every writer moved below
  64 writers and 1.13 to 1.94 times at 64 - sixteen a CPU - its writers' sleeps
  flat and its slowest writer within 5% of the mean from 16; measured on no
  cluster yet. `OSOE_TURNOVER_WAKE=all` is the old rule, and `hand-on`, a
  chain of wakes, moved 5 to 40% fewer bytes below 64 writers there (P2,
  `docs/design/evidence/hand-on-wake/`).
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
  choose by argument count, and a program may still declare or define one by
  its plain name, as a profiling wrapper written as the specification's example
  in §10 does: the macro leaves it a declaration of the function. The
  deprecated `shmem_wait` and `shmem_wait_until` on a `long`, whose names C11
  gives the type-generic waits, a wrapper writes in parentheses,
  `(shmem_wait_until)`.
- **MPI in the same program works, started by `srun` with Slurm's PMIx or by
  `mpirun`.** Tried in the development VM (aarch64, Debian 13), with Open
  MPI 5.0.7 as the distribution packages it and Slurm 24.11.5's PMIx plugin:
  `MPI_Init` and `shmem_init` in either order and ended in either order,
  `MPI_Allreduce` and `MPI_Sendrecv` on the symmetric heap and on statics, an
  MPI window over the heap beside `shmem` puts into it, `shmem_finalize` and
  `shmem_init` again with MPI running, `MPI_THREAD_MULTIPLE` beside
  `shmem_init_thread`, up to 16 PEs, on one node and in two or four logical
  nodes, and sixty starts in a row without a failure. Build with `OSOE_CC=mpicc
  oshcc`, which keeps osoe's `shmem.h` ahead of the one Open MPI installs beside
  `mpi.h`. Start with `srun -K1 --cpu-bind=none --mpi=pmix_v5`, where Slurm
  gives osoe and Open MPI what each reads. `mpirun` gives osoe none of the
  variables of "Starting PEs without a launcher", and a program it starts is
  refused at `shmem_init`; a wrapper makes them from Open MPI's
  (`docs/design/evidence/v0.2-step4/2026-10-21-t44/open-mpi-bootstrap.sh`, for
  one machine, its logical nodes set by `T44_NODES`; several machines are
  untried). Under `osoe-launch` every PE can be an MPI singleton, a world of one
  rank, which gives no MPI between the PEs. Not tried: another build of Open
  MPI, MPICH, x86-64, a program linked by lld, and speed.
- **Beside MPI the deadlock detector proves nothing.** Open MPI's threads are
  threads of the program's own outside the library's waits, and a program with
  one is never judged deadlocked (the bullet on a program's own deadlock,
  above): the thread `MPI_Init` starts, until `MPI_Finalize`, and in Debian's
  build one that UCX's `libucs` starts as the program loads, in every program
  that links `libmpi`, MPI started or not (UCX's virtual file system). A
  deadlock in osoe alone then hangs until the launcher's time limit, if it has
  one, where the same program built without Open MPI was ended in 4 s at
  `OSOE_DEADLOCK_SECONDS=3`; `UCX_VFS_ENABLE=n` stops UCX's thread and gives
  the detector back before `MPI_Init` and after `MPI_Finalize`. A deadlock
  across the libraries, a PE in `MPI_Barrier` and the rest in
  `shmem_barrier_all`, is found by neither. Under `mpirun`,
  `shmem_global_exit(status)` ends the job with status 1 and `prterun`'s
  complaint of ranks that left without `MPI_Finalize`; under `srun` and
  `osoe-launch` it ends it with `status`, and `MPI_Abort(status)` carries its
  status under all three. An MPI singleton leaves a 16 MiB sparse
  `sm_segment.*` file in `/dev/shm`. The package that installs Open MPI also
  installs an `oshcc`, an `oshrun` and an OpenSHMEM `shmem.h` of its own, to be
  kept behind osoe's. The record is
  `docs/design/evidence/v0.2-step4/2026-10-21-t44/`.
- **`shmemx.h` is empty.** The cache routines (`shmem_set_cache_inv` and five
  more) were deprecated in 1.3 and left the specification after 1.4; osoe keeps
  them as routines that do nothing, for programs that still call them, which is
  right on a cache-coherent machine, as every one osoe runs on is. They are
  declared in `shmem.h`, where those programs look for them, though §5 allows
  that header only the specification's interfaces; `shmemx.h`, where §5 puts
  the rest, would take them only renamed `shmemx_`.

## Toward RDMA: a RoCE backend

osoe has no RDMA backend, and building one is no part of 0.1 or 0.2: no RoCE
hardware is at hand. What the design guarantees is
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
  Soft-RoCE (`rdma_rxe`), for function, not performance: possible later work,
  should hardware still be far off.

## How osoe is tested

- **Four build presets** - `debug`, `release`, AddressSanitizer with
  UndefinedBehaviorSanitizer, and ThreadSanitizer - each with the whole suite
  run before every merge, on the exact tree that merges: about 1,200 tests in
  release, and the model checker's 47 tests beside them, which run in that build
  only; fewer under ThreadSanitizer, which cannot see an ordering that runs
  through another process, and so leaves most tests across nodes to the other
  three.
- **Progress helpers.** The suite runs at `OSOE_PROGRESS_HELPERS=0`, `2` and
  `3`, and with the setting unset, the default `auto`, by the environment
  `ctest` passes on to every test, in every preset.
- **A build by hand for the one-node quiet's fence.** On aarch64 the counters a
  `shmem_quiet` bumps keep a store before a later load in the hardware, at the
  compiler's default `-march`, so nothing there fails without the fence that is
  all of quiet in a job of one node; built for Armv8.3, whose acquire loads are
  `ldapr`, a store-buffering litmus through `shmem_quiet` finds the forbidden
  outcome without it. `shmem.quiet.litmus.L2` runs that litmus, and skips where
  it cannot show the fence - every preset on aarch64 - so on a CPU with
  `ldapr`:

  ```sh
  cmake --preset release -B build/armv8.3 -DCMAKE_C_FLAGS=-march=armv8.3-a
  cmake --build build/armv8.3 -j4
  ctest --test-dir build/armv8.3 -R '^shmem[.]quiet[.]litmus'
  ```

  On x86-64, where only the fence's `mfence` orders the two, it runs in every
  preset, and it passed natively on Google Cloud's x86-64 nodes in tier 2, the
  tests of jobs on one machine (design §8.2 sets out the tiers; 2026-10-28).
- **The measured link.** Every test of more than one node runs with
  `OSOE_TEST_PRINT_LINK=1`, so that its output gives the link the job measured
  as it started, as the three `OSOE_LINK_*` settings that run it again on the
  shapes that link chose: a failure tied to one of them can be repeated.
- **ThreadSanitizer's verdict.** Under it every test runs with the tool's
  `exitcode=0`, `abort_on_error=1` and a `log_path` of its own
  (`cmake/OsoeThreadSanitizer.cmake`): the tool leaves each process its own
  exit status and turns a crash it catches into an abort, so that `ctest`
  judges every test there as it does in the other presets, and it writes its
  reports to files named for the test, which a last test, `tsan.reports`,
  judges at every helper count, none included - its failure the run's, and a
  canary's race, which it must find first, showing that the files are written
  (`cmake/thread-sanitizer-gate.py`). With no helpers there is no report at
  all. With two or three, about a hundred or more - at three, on 0.2.0's tree,
  145 in 31 of the tests across nodes - each a put or an atomic a helper
  applies in a PE's heap mapping against a local thread's access to the same
  place, or another helper's put there in an earlier collective. The ordering that makes them safe runs
  through another process, which the tool cannot see - the reason that preset
  leaves out 347 of the 633 tests labelled `internode`, `shmem.internode`'s
  maps among them (design §8.2) - and with no helpers the
  one agent applies every peer's puts and has met the local thread through the
  barrier's handoff, which the tool does see. So a report passes only as one
  of those: a data race, naming a helper's thread, in a heap mapping; and of
  the three quarters in the words the library keeps for its collectives at the
  end of that mapping, only at the places, and against the accesses, the gate
  names with the reason each is ordered - a team number's words against their
  zeroing when a split took the number, and a parent's grant words, a child's
  lane word or a piece in a lead's small slot against the collective before's
  or their reader's clearing of it, and a test's own look at a lead's grant
  fields between two barriers (since helpers run by default, 2026-10-28). A crash, a thread leak,
  a report of any other kind, and a race no helper made fail it, wherever they
  are: each was planted and failed, and with the gate before 2026-10-11, which
  read `ctest`'s log, a failed check or a crash beside a race passed
  (`docs/reviews/archive/v0.2-before-part4-review.md`, C9-1). (Until a Träff's
  reduction's grant gave each receiver a field of its own, two helpers could
  also write its head at once, the same bytes from each, and that word was
  about half of the reports: `docs/decisions/2026-10-10-grant-fields.md`.) One
  test, which reads back 64 MiB a helper landed, takes one to two minutes there
  in the tool's handling of eight million such reads, and is given five. The
  preset runs without `shmem.helper-wake`, which it leaves out for that reason.
  That test counts each owner's wakes - a helper's, and in a second run at no
  helpers the lead's agent's - putting to a peer only once the owner of its
  wire has raised the flag it raises before it sleeps, and checks that the put
  claimed that owner's wake itself: a wake sent to another owner or held back
  fails it, and so does an owner that spins after raising its flag for longer
  than an acknowledgement's round trip, a shorter spin being caught alone but
  less surely under load. Beside four busy loops on four cores it passed 20
  runs of 20, at two helpers and at none, in the release build and under
  AddressSanitizer, where counting wakes without the flag failed 8 of 20 in the
  release build and 9 of 20 under AddressSanitizer
  (`docs/decisions/2026-10-10-helper-wake.md`). Those counts cannot see an
  owner that spins before it raises that flag - the flag raised late still
  finds its waker - and `shmem.late-flags` is for it, at two helpers and at none: each
  owner counts the flags it raises more than 10 µs after the pass that found
  nothing to do, and under a trickle of 300 puts 2 ms apart every node's owners
  must raise at most one late in four of their wakes, and pass at most twelve
  times a wake. As built they raise none late and pass five times a wake on the
  putting node, about twice a put on the others; with the spin round nine's
  review planted, a poll of up to 200 µs before the flag, they raise one late at
  every wake and every put received
  (`docs/decisions/2026-10-27-layout-42.md`).
- **Two builds.** The single-node build runs the same suite less the
  633 tests labelled `internode` - those of a job of more than one node, of
  the internode backend's own files, and of io_uring and futex2 - and less
  the full build's check that the single-node build configures, and with
  five of its own, in the same four presets, on Rocky Linux 9 and in the
  development VM with liburing hidden: 580 of the full build's 1,214 tests in
  the debug preset and its five, two fewer without a C++ compiler, which the
  headers' C++ test needs. `ctest -L internode` on the full build runs the
  tests it lacks but that check, `shmem.single-node.configure`.
- **Model checking.** Every lock-free and futex protocol - waking the
  progress thread, waits, the barrier tree, pipelined collectives and the lanes
  their large pieces go through, Bruck's allgather, the scans, get completion
  under `SHMEM_THREAD_MULTIPLE`,
  rendezvous, abort, the proof a deadlock is found by, a progress helper's
  requests and its abort, the deprecated active-set collectives' counted
  signals, and the teams' - a team
  block's life and the hold of its release, a split's pending numbers, a pool
  found empty, the posted arrivals - is checked
  exhaustively in small configurations by an explicit-state model checker
  written for them (`tools/modelcheck`). Six of the models, those of the
  `SHMEM_THREAD_MULTIPLE` designs, run only in a build configured with
  `-DOSOE_MULTIPLE_MODELS=ON`, which no preset sets; get completion's one case
  that a `SHMEM_THREAD_SERIALIZED` program meets runs in every one. Each model
  also carries deliberately broken variants that must fail: a model that cannot
  fail proves nothing. The deadlock detector's use of the helpers' answers -
  each answer naming the request posted last, the helpers' work counted in the
  node's activity, a pass after the request - was once held by
  `deadlock_owners_model.py` alone: each could be removed with every detector,
  helper and stress test passing at two helpers
  (`docs/reviews/archive/v0.2-step4-correctness.md`, Q2-3). Now a true
  deadlock at two helpers, with one helper made to misbehave in the way its
  clause guards against (`OSOE_TEST_STALE_HELPER`, `OSOE_TEST_BUSY_HELPER`,
  `OSOE_TEST_SLOW_HELPER`), must go unproved, as `shmem.deadlock.helper-stale`,
  `-busy` and `-slow` require, and with its clause removed the detector proves
  it and the test fails. The third is held by the order alone: a helper's view
  reads the open buffers' heads, where the PEs reserve what they post, so an
  answer given before the pass says of that what one given after it would, and
  the test, with the pass held back, sees only that the answer came first. A
  team's local release is held until its RELEASEs have left the control queues,
  so that its members do not leave, and the block is not claimed again, with one
  of the old team's queued (design §3.8); it once had neither test nor model,
  and the suite passed with the hold removed
  (`docs/reviews/archive/phase1-step7-slice3-correctness.md`, G1). Now
  `team_block_model.py` queues the RELEASE and `no_hold` fails it, and
  `shmem.team-release.held.0-1-2`, with node 0's control wire to node 1 shut
  for half a second after a RELEASE is queued there
  (`OSOE_TEST_HOLD_RELEASE`), counts the release held and, with the hold
  removed, ends the job naming the block at its first round. The
  active-set collectives' model (`active_set_model.py`, of `active-set.c`) runs up to ten members through up
  to seven calls back to back on one pSync, over a network that lands each put
  and atomic add in any order a fence, a quiet or put-with-signal allows, with
  members that share a node and members that do not. The checker runs a model
  under sequential
  consistency or x86-TSO (some under both, among them the teams' four models,
  those of the lanes and of a scan's pieces, and the active-set collectives',
  each store as the library orders it
  and a `release` store buffered as x86 buffers it), and both keep a thread's
  loads in order, which
  aarch64, the machines osoe is built for, does not: the "blocked in" record's
  model passed while its routine name, then plain characters, could be read torn
  on aarch64. That name, a plain field rewritten while another process reads it,
  was a finding of the review before the first release (`docs/reviews/archive/`);
  it is now four atomic words, and
  `shmem.blocked-record` runs the record's own writer against both of its
  readers on the machine. In an optimized build it fails when the library's
  reader loads the name plainly again; a plain writer, or `osoe-stat`'s reader,
  tore nothing it could see on these machines.
- **Sandia OpenSHMEM's test suite**: 161 of its 165 programs passed on 0.2.0's
  tree at four logical nodes of two PEs in the development VM, with no progress
  helpers and with two, and the same day at 4 × 2 on the Pi cluster, 8 PEs, on
  its code before the version was changed (2026-10-30), as on the tree tagged
  0.1.0 - on the Pi with `bigget` at a smaller loop count, since its default is
  16 GiB of gets a PE over gigabit Ethernet; natively on x86-64, on eight
  Google Cloud nodes at 2 PEs a node (2026-10-28), 159 of the 163 programs of
  the cluster kit's sweep, which leaves out two variants, the same four short.
  The single-node build, at v0.2
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
  OpenSHMEM examples, built with `oshcc` - on the Pi cluster on 0.2.0's code
  (2026-10-30), IS verifying at classes A and B, PRK's three kernels
  validating, ISx weak and strong, and 64 of the 72 examples, the other eight
  as the runbook says (`docs/pi-cluster.md`).
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
  every atomic, transfer and collective, a block put to one node and read from
  it by a third after a quiet, and all of them at once on teams out of step -
  every result checked against an oracle, and every oracle checked by a planted
  defect it must catch. A build of it that counts every call
  (`osoe-pacer-counted`, built by the full build where Python 3 is found) shows which of the
  library's 1,794 routines a run reaches.
- **A Raspberry Pi 5 cluster** over gigabit Ethernet, its four nodes running
  with TCP and generic segmentation offload off. Before, one node's spurious
  retransmissions stalled collectives of some sizes for up to 52 s at a time
  (F1); turning its offload off ended the stalls in the runs that followed.
  Those runs came just after every node rebooted, so a control in October
  told the two apart: with the offload off, F1's sizes stalled nowhere 1.4
  and 23.7 hours after boot; with it on, 13 hours after, calls stalled 45 ms
  at them - not F1's seconds, and with no retransmission timeout. F1 is not
  explained - three hours of `mix` with the offload off still counted 87
  retransmission timeouts (`docs/pi-cluster-results.md`, "F1, investigated",
  the timeouts in "The pacer's fourth phase", and "H29 and H22"). There an 8-byte put with
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
only there, and one marked *checked between nodes* only between them.
`SHMEM_INFO` prints every variable of this section's tables from the library
itself. An `OSOE_` name the library does not read changes nothing, and the
job's rank 0 says so once, naming the one it reads a letter or two away where
there is one - `OSOE_DEADLOCK_SECOND is not a variable osoe reads, and changes
nothing: did you mean OSOE_DEADLOCK_SECONDS?` - from a list of every name osoe
and its tests read, which a test, `shmem.settings-known`, holds to the tree. In
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
| `SHMEM_VERSION` | unset | Set: every PE prints osoe's version and the specification's at start-up, as `osoe 0.2.0, OpenSHMEM 1.6`. |
| `SHMEM_INFO` | unset | Set: each node's first PE prints the variables osoe reads, with their defaults, under a first line naming osoe's version. |
| `SHMEM_DEBUG` | unset | Set: every PE prints what the library set up for it - its rank and node, its heap's size and address, its control segment's name, and in a job of more than one node the link its large collectives choose their shapes from: measured, with what measuring it cost the PE's start, or set. There each node's lead also prints the limit it set on the unsent bytes of its connections (`OSOE_WIRE_UNSENT_BYTES`), once every owner of its wires has applied it, whether it probes them with TCP keepalive and checks for a silent peer (`OSOE_KEEPALIVE`), and how long its mesh took, from the rendezvous's release to its last connection. |

**Limits**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_MAX_TEAMS` | `256` | Teams one PE may be in at once, the three predefined teams included - the world, the node's shared team, and one the library keeps for `shmem_finalize`: `4` to `1024`. A destroyed team gives its number back. Each number costs every PE about 25 KiB of address space at the other variables' defaults on a few nodes - counters and the slots of its small collectives, which only a team's leads touch, and the same for its scans, which every member of a team that scans does - 28.1 KiB at 64 nodes and 29.6 KiB at a thousand, collect's exchange's slots growing with the nodes to 64. *Checked*. |
| `OSOE_TEAM_BLOCKS` | `1024` | Teams a node can hold at once, the three predefined teams' included: the node's pool of team blocks in its control segment, a kilobyte each of `/dev/shm`, allocated at start, and about 760 bytes each of the memory of the node's first PE, from which the local lead on each node of a created team of more than one PE takes one, and to which the last of its members there gives it back; a team of one takes none. `8` to `65536`. With none left, a split that needs one on the node returns nonzero on every PE of its parent team, naming the variable and the node. Nodes may differ; a node's PEs may not. *Checked* among a node's PEs. |
| `OSOE_MAX_CONTEXTS` | `64` | Contexts a PE may have at once, the default context included: `1` to `4096`. A creation that finds none free returns nonzero, as the specification lets it, and the PE says so once, naming this variable. *Checked* among a node's PEs. |
| `OSOE_MAX_THREADS` | `16` | Live threads of a PE that may have called the library, the main thread included: `1` to `1023`. A thread's place is taken at its first call after `shmem_init`, not counting queries such as `shmem_my_pe`, and freed when it exits, not when the call returns; a call that finds none free ends the job, naming the variable. Each place, and the agent's, has a lane for the large reductions and broadcasts its thread leads and the large scans it makes, 1,153 KiB of address space at the default fan-out and pipeline depth to 512 nodes, 128 KiB more for each doubling of the nodes past it, which a thread that does neither never touches: 19.1 MiB a PE at the default - but for the link's measurement as a job starts (`OSOE_LINK_HOP_NS`), which writes 64 KiB of the first lane of node 1's first PE and a page of the first lane of each of the next nodes' up to the fan-out. *Checked* among a node's PEs. |
| `OSOE_GET_LANDING_BYTES` | a node's `4194304` over its PEs | Bytes of each PE's landing zone for the replies of its gets from other nodes, a multiple of `1024` up to `67107840`: past it, a new get waits for earlier ones to land. Unset, the node's 4 MiB split among its PEs in whole KiB, no less than `262144` nor more than `1048576` a PE - 1 MiB to each of 1 to 4 PEs a node, 512 KiB to 8, 256 KiB from 16 - each node from its own count (P32). *Checked* among a node's PEs. |

**Collectives**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_COLLECTIVE_FANOUT` | `8` | The ceiling of the tree the node leads meet in: `2` to `32`. A barrier's tree and the small class's take it whole, and a scan's under `fixed`; under `link` and `tree` a scan's tree keeps 0.1.0's fan-out of 4, or the ceiling where that is less, since a scan cannot change its tree from call to call; a large reduction's or broadcast's takes the fan-out up to it that the link makes cheapest for its size (`OSOE_COLLECTIVE_SHAPE`), since over Ethernet the fan-out is a bandwidth choice at large sizes, a node sending a copy to each of its children over its one link, and a latency choice at small ones. The layout of every team number and lane is sized for it: a lane has the ceiling + 1 rows, or past 512 nodes at the default the ⌈log₂ n⌉ that Träff's reduction takes, a row for each round of a half, so that it runs on up to 65,536 nodes at any ceiling and the ceiling does not limit it. *Checked*. |
| `OSOE_COLLECTIVE_PIPELINE` | `8` | The capacity: the most pieces of a large reduction, broadcast or scan each node or member keeps in flight up and down its tree, `1` to `8`, and what every lane's slots are sized for - each round of Träff's reduction keeping that many again for each of a lane's rows its rounds leave it, 32 pieces on three or four nodes and the capacity from 17. A large reduction or broadcast keeps the capacity in flight under `link` as under `fixed`; a scan keeps it under `fixed`, and under `link` and `tree` 0.1.0's two, or the capacity where that is less: on the Pi cluster four in flight made a scan over 16 PEs on four nodes 1.30 times faster at 1 MiB but 6.6% slower at 64 KiB. A reduction's or a broadcast's piece carries 16 KiB, its header in the lane's slot beside it; a scan's is sized to it, at most 16 KiB. The default was 4 until 2026-10-28 and has been 8, the most allowed, since (P37, the author's word of 2026-10-25): on eight nodes of Google Cloud's link a window of 8 made every broadcast from 256 KiB 17 to 27% faster than 4 (`docs/fast-link-results.md`), at the price of doubling every lane (`OSOE_MAX_THREADS`). *Checked*. |
| `OSOE_SMALL_COLLECTIVE_BYTES` | `1024` | The largest reduction, broadcast or scan that moves in one piece through slots its team keeps: `128` to `16384`. A larger one goes in pieces through lanes the threads that lead it lend it - every member's thread, for a scan - and a reduction's or scan's child may wait for its parent's, once - up to a hop, or a round trip when the parent enters after it. Each team number keeps the slots - one for each child position of the tree and one for the release, for its reductions and broadcasts and again for its scans - so each byte of the value costs every PE eighteen bytes a number at the default fan-out of 8: lower it for memory, raise it to keep a few kilobytes' reduction or scan in one hop. *Checked*. |
| `OSOE_COLLECTIVE_SHAPE` | `link` | `link`, `tree` or `fixed`: whether a large reduction's or broadcast's shape is to be the one its size makes cheapest on the link - a tree's fan-out and window, or for a reduction Träff's reduce-scatter and allgather - chosen alike on every node from the link's three numbers; the same among the trees alone, Träff's turned off by name; or `OSOE_COLLECTIVE_FANOUT` and `OSOE_COLLECTIVE_PIPELINE` for every call, as osoe 0.1.0 did, which measures nothing - the setting that makes a run repeat its results, bit for bit within a version (as the link's three numbers set, `OSOE_LINK_*`, also do under `link` and `tree`), or takes 0.1.0's fan-out and pipeline with `OSOE_COLLECTIVE_FANOUT=4 OSOE_COLLECTIVE_PIPELINE=2`; with no link known, it also leaves each data wire's unsent limit at its 16 KiB floor unless `OSOE_LINK_*` are set (`OSOE_WIRE_UNSENT_BYTES`). The chooser's costs were fitted to the Pi's link: on eight nodes of Google Cloud's 23 Gbit/s link a fixed fan-out of 4 with a window of 8, even at that floor, ran reductions and broadcasts from 256 KiB to 4 MiB 17 to 26% faster than the shapes `link` chose at the capacity of 4 it had then (H13, 2026-10-20, `docs/fast-link-results.md`); at a capacity of 8, which that measurement set by `OSOE_COLLECTIVE_PIPELINE=8` and the default has been since 2026-10-28 (P37), `link`'s broadcasts came within 3% of it and its reductions from 1 MiB 24 to 33% behind, and the one job of ten whose link read a message cost of 21 µs, not 23 to 45 - by the trimmed mean of the tries' differences that the least of them has since replaced (the `OSOE_LINK_*` row) - took Träff's reduction and ran 1 and 4 MiB 2.4 and 2.6 times faster (P37, 2026-10-28). A barrier and the small class keep the ceiling's tree under each, and a scan 0.1.0's shape under `link` and `tree` (`OSOE_COLLECTIVE_FANOUT`). *Checked*. |
| `OSOE_COLLECTIVE_PLACES` | `link` | `link`, `members`, `totals`, `scan` or `exchange`: how a `collect` of differing blocks on a team spanning nodes finds each PE's place in the result. `link` takes, where every node's PEs are one run of the team's ranks - a block distribution - the way of the three below that the link makes cheapest, as a large collective's shape is chosen, the exchange on no more than 16 nodes: on the Pi's link the exchange to 16 nodes, each node's total through the tree to 25 and the scan beyond, and on a loopback's the exchange to 16, each node's total to 128 and the scan beyond (`shape_timing.py places`). The 16 is H24's: the exchange measured the fastest of the four at 8 and 16 logical nodes and slower than the scan at 32, on one machine whose CPUs carry every node's messages at once, which overstates its cost (`docs/design/evidence/v0.2-step4/2026-10-13-h24/`) - where the cost model alone takes it to 60 nodes on the Pi's link; on eight nodes of Google Cloud's cluster, at 8, 16 and 32 PEs a node, the exchange was the fastest of the four and `link` took it (H25, 2026-10-20) - past eight nodes the 16 is untested; each node's total through the tree where no link is known (`OSOE_COLLECTIVE_SHAPE=fixed`, or one node); and every PE's length wherever some node's PEs are not one run. The others force one way, for the tests and for measuring the crossover among them: `members` every PE's length, on any team; `totals` each node's total; `scan` a scan of the nodes' totals among the node leads, each learning only its own node's place and the total, and the data's steps carrying the byte ranges they send - where the runs of ranks meet the nodes in their order or its reverse, as a block distribution's do under a positive or a negative stride; `exchange` each node's total put by its lead straight to every other lead, one hop where the tree takes two levels and more, on a team of up to 64 nodes. A way the team does not allow falls back, `scan` and `exchange` to `totals` and `totals` to `members`. *Checked*. |
| `OSOE_LINK_HOP_NS`, `OSOE_LINK_MESSAGE_NS`, `OSOE_LINK_BYTES_PER_SECOND` | not set | The link the shape is chosen from, set by hand in place of its measurement - for tests, and for a fabric known in advance: the one-way time of a small message between two nodes' leads, through both progress threads (`1` to `10000000000` nanoseconds); what a lead's loop spends on each message it sends (`0` to `1000000000` nanoseconds); and the bytes a second a node's link carries each way (`1` to `10000000000000`). All three or none: one alone is refused by name. Not set, a job of more than one node with the shape `link` or `tree` measures the link at the end of `shmem_init` - the root's lead, through the data path with every progress thread running, against its first children's: the hop from round trips, the bandwidth from streams, the message cost from one message to each child against one alone, the least of fifteen tries of each, since 2026-10-28 (a wake late or a page touched first only lengthens a try; every message cost quoted below was read before, by the trimmed mean of the tries' paired differences) - and every PE takes the root's numbers before `shmem_init` returns. On the Pi cluster that cost a start 27 ms in v0.2 step 2's tree (`docs/decisions/2026-10-06-tree-shape.md`), the hop and the bandwidth agreeing within 8% start to start, and has not been timed there since P38 (below) lengthened it on three nodes or more; the message cost there varied from 0 to 21 µs between starts, which moves no choice its four nodes have. On Google Cloud's 23 Gbit/s link it cost 3 to 5 ms of a start on two and three nodes; the hop agreed within a fifth start to start, but the bandwidth read 1.9 to 5.1 GB/s of a link that carries 2.85, its streams too short to time such a link, and the message cost 0 on two nodes, whose root has one child to time it by, and 0 to 159 µs on three (2026-10-20, trimmed mean). So a link on which the long stream's bytes beyond the short one's take less than a millisecond gets streams that much longer, up to 64 times, and each child comes to a timed message from the same idle as the others (P38): over 30 starts on the development VM's loopback the middle 80% of the bandwidths measured narrowed from 1.7 to 5.9 GB/s to 3.6 to 6.1 on two nodes, and from 1.9 to 5.9 to 4.2 to 7.7 on three, the highest message cost on three (trimmed mean) from 15 µs to 5, for 2 to 3 ms more a start; and on three nodes or more each of the message cost's fifteen tries takes a round trip more, 5 ms more a start on tier 3's emulated gigabit at three nodes. On Google Cloud's link again (P38, 2026-10-28) it cost 9 to 20 ms of a start and was no steadier: the bandwidth 0.88 to 5.68 GB/s from job to job, the hop 37 to 120 µs, the message cost 0 on two nodes and 0 to 151 µs on three (trimmed mean). Where the link is slower its streams carry 1.9 MiB, so a slower link pays more - about 170 ms at 100 Mbit/s - and set numbers or the shape `fixed` skip it. Its puts - 136 on two nodes, 243 on four, 398 from nine at the default ceiling, more where the streams are longer - count in the transport counters of the root's lead as a program's would. `osoe-stat` shows the link, measured or set, and so does `SHMEM_DEBUG`'s start-up line. *Checked*. |

**Start-up, failures and inspection**

| Variable | Default | What it does |
|---|---|---|
| `OSOE_NO_PROGRESS_TIMEOUT` | `0` | Seconds a wait on the library's own progress - acknowledgements, buffer space, a reply, the rendezvous - may see none before it ends the job, naming what it waited for; `0`, never. A start-up wait on another node has two seconds more, and a PE's wait on its lead in the rendezvous four, so that a node whose own PE never calls `shmem_init` ends first, naming the PE's rank. A program's own waits and collectives are not timed. |
| `OSOE_KEEPALIVE` | `60,10,6`, or `0` where every node has a loopback address | `IDLE,INTERVAL,COUNT`: TCP keepalive between nodes, in seconds, seconds and probes, `1` to `32767`, `1` to `32767` and `1` to `127`. IDLE + INTERVAL × COUNT, 120 s by default, is also how long a node's kernel may leave data sent to it unacknowledged before the node counts as vanished. `0` for neither. Not set, a job whose every node has a loopback address - `osoe-launch`'s logical nodes - has neither: with no link between its nodes to cut, and a kernel that closes a dead node's connections itself and answers for a stopped one, a probe can find nothing there, and an overloaded machine's receive backlog drops them. Nodes with addresses of their own keep both, tier 3's namespaces on one machine among them. `SHMEM_DEBUG` says which a node chose. |
| `OSOE_DEADLOCK_SECONDS` | `10` | How long a job whose every PE waits in the library, with nothing moving on any node and nothing in flight, is watched before it ends as deadlocked, each node naming where its PEs wait: `1` to `86400`, or `0` for never. |
| `OSOE_MESH_SLOT` | `100` | Microseconds between the slots of the mesh's start-up schedule, `0` to `100000`: each lead calls one place further round the circle a slot, its calls of a slot begun no earlier than the slot's time, counted from the rendezvous's release, so that each lead is called by about one other at a time; `0`, no slots, each call as soon as fewer than eight are in flight. The schedule's ⌈(n - 1)/2⌉ slots are 3.2 ms at 64 nodes and 0.2 s at 4,097 at the default, the least it takes; calls slower than a slot's share of the eight in flight set the pace instead. Nodes may differ. |
| `OSOE_MESH_DATA_WIRES` | `eager` | `eager` or `on-demand`: when a node pair's data wire, which carries every put, get and atomic between the two nodes, is made. `eager` makes every one at start-up, as osoe always has. `on-demand` makes the control wires alone at start-up - eagerly to every node, so that start-up still proves every node reachable, and the barrier, the deadlock detector, the abort and keepalive keep theirs - and a pair's data wire at the first traffic between the two nodes, which waits for it meanwhile: the node the mesh's schedule has call the other dials, the other asking it to over their control wire, and each lead's listener stays open for the job's life. What it saves is the data wires' sockets and their part of start-up, for a program whose nodes each talk to few others - which still makes the collective tree's data wires, one a node, at its first collective that carries data: `shmem_malloc`'s check of its arguments, and the end of `shmem_init`, which measures the link between nodes 0 and 1 and hands it out unless the `OSOE_LINK_*` settings give it. A data wire that cannot be made is met at the first traffic of its pair rather than in the start-up's mesh: its wait says what it waits for after 30 s and every minute after - the node, its address and the last call's error, as start-up's calls do - and ends the job only under `OSOE_NO_PROGRESS_TIMEOUT`. Measured on no large cluster yet, which is why it is not the default. *Checked* between nodes, by every lead before any wire is made: a job whose nodes differ is refused, naming the first node that differs from node 0 and how each makes them. |
| `OSOE_PORT_RANGE` | `17000-26999` | `LOW-HIGH`, within `1024-65535`: the ports a node's lead may listen on for the rendezvous. Each node tries 32 of them, drawn from the job's ids, and a node that finds none of its 32 free ends the start, naming the range. |
| `OSOE_MESH_PORT_RANGE` | not set | `LOW-HIGH`, within `1024-65535`: the ports a node's lead may listen on for the connections between nodes, which it tells the others in the rendezvous, so nodes need not agree on it - for a site whose firewall opens the nodes' ports by range. Not set, the kernel chooses the port, as it chooses a connection's own. A lead takes the first free port of the range from one its job and node pick, and refuses to start naming the range where none is free; best apart from `OSOE_PORT_RANGE`, whose candidates it may otherwise take. `SHMEM_DEBUG` has each lead say the port it took. |
| `OSOE_JOB_SECRET` | unset | 32 hexadecimal digits: a secret every node proves it knows when it joins the job, never sent. A value that is not 32 hexadecimal digits is refused by name, but an empty one is taken as unset, without a word: a wrapper whose secret came out empty starts the job unsecured. *Checked* between nodes: where one node has a secret and another has none, or a different one, the node that sees it ends naming the variable and both nodes; a node's PEs are not compared, and only its lead's is used. |
| `OSOE_HEAP_BASE` | chosen | The address every PE maps its heap at, a multiple of the page size, in place of the library's own candidates - digits, in decimal or after `0x` in hexadecimal, with no sign: a node whose lead cannot map its heap there refuses to start, naming the address and the kernel's reason - or, under ThreadSanitizer, the tool's: it refuses an address outside its application memory, as it does the library's first candidates, which the lead passes over. The node's first PE's value is the node's. |
| `OSOE_SHM_CAPACITY_CHECK` | `1` | `0`: a node whose heaps would not fit `/dev/shm`'s free space is warned, not refused. |
| `OSOE_KEEP_CONTROL_SEGMENT` | `0` | `1`: the control segment keeps its name in `/dev/shm` until finalize, so a crash leaves it for `osoe-stat` to read; `osoe-stat --gc` removes such segments once their PEs are gone, as it does a [failed start's leftovers](#names-a-failed-start-leaves). |

**The progress threads**: each PE's agent thread, which on a node's first PE
moves every message of the node, and the helpers that can share the work

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AGENT_SLICE_US` | `100` | The scheduler slice each agent thread asks for, in microseconds: `100` to `100000`, or `0` for the kernel's own. A short slice lets it preempt sooner when it wakes on a core it shares - the same fair share, no priority over anyone. Linux 6.12 and later; an older kernel ignores it. It is a trade, measured on the Pi cluster against `0` with 16 PEs on four nodes. Under Slurm's default binding it gave a pipelined mailbox 1.3 to 1.7 times the messages a second, a 64 KiB reduce 24% less time and a 4 KiB alltoall 10% less, and cost the barrier 8% and an 8-byte reduce or broadcast 11 and 13%. Unbound (`--cpu-bind=none`) it left a 64 KiB reduce as it was, gave the mailbox 2% more and the alltoall 8% less time, and cost the barrier 2 to 4% and an 8-byte reduce or broadcast 10 to 12%. A job that fills its nodes gets Slurm's binding without asking, which is why `100` is the default. |
| `OSOE_AGENT_CPUS` | `task` | Where each agent thread, and each progress helper, may run: `task`, on its own PE's CPUs; `node`, on any CPU any PE of its node was bound to - the job's own cores there, never another's. The PEs' threads keep their binding either way. On the Pi cluster, bound, `node` gave the mailbox a fifth more messages a second where the slice was not in force (`OSOE_AGENT_SLICE_US=0`: 42,900 to 51,300) and nothing measurable beside it (58,500 against 57,500 to 62,900), and beside the slice made the barrier and an 8-byte reduce or broadcast 7 to 15% slower and a 64 KiB reduce 18%. The thread is named `osoe-agent`, as `top -H` and `ps -L` show it. |
| `OSOE_PROGRESS_HELPERS` | `auto` | Threads of each node lead's process beside its agent that hold the node's data wires: `0` to `64`, or `auto`. The peer node that is p-th in node order, counting from 0 and leaving the node itself out, is helper 1 + (p mod the count)'s; the lead's agent keeps every control wire, which carry the barrier's and the deadlock detector's messages. Each helper is started after the agent, placed and given a slice as the agents are (`OSOE_AGENT_CPUS`, `OSOE_AGENT_SLICE_US`), named `osoe-helper-N`, and stopped at `shmem_finalize` once it has closed its wires. A peer's traffic is always one thread's, so helpers spread a node's traffic with several peers at once, not one stream; on four nodes of the Pi cluster two of them made broadcasts and alltoalls slower, bound or not, and a 64 KiB reduction faster only under Slurm's binding ("Without progress helpers one thread a node moves every message between nodes", Limits), and on eight nodes of Google Cloud's 23 Gbit/s link two helpers brought every stream to the provider's egress ceiling and the alltoalls a quarter faster, but halved the mailbox where a node ran more PEs than vCPUs (`docs/fast-link-results.md`, 2026-10-20) - so the author decided on two by default on a job of three nodes or more where a node runs no more PEs than it has CPUs (P36), once a site's binding was measured too (H26, 2026-10-28): with Slurm binding a core a PE two helpers took the streams a fifth faster and halved the mailbox, with one CPU a PE little either way. The default is `auto`, which applies that rule: two where the job has three nodes or more and the node no more PEs than the CPUs its machine has online - whatever share of them the job may use - and none otherwise, each node by its own PEs, or, where `osoe-launch`'s logical nodes share one machine, by the machine's, all its logical nodes' (`OSOE_MACHINE_PES`, below), and `SHMEM_DEBUG` has each node's lead say the count it took and why. `0` keeps one thread a node, for a job that lives by its message rate: at a core a PE the helpers halved the mailbox. A count above the node's peers leaves the extra helpers idle, each with its ring and receive buffers. Under `OSOE_AGENT_CPUS=task`, the default, the helpers run on the lead PE's CPUs as its agent does: where Slurm binds each task to one core, the lead's main thread, its agent and every helper share that core, and helpers add threads but no CPU - `node` lets them run on any CPU of the node's tasks. `osoe-stat` shows each owner of the node's wires - the agent and every helper - with its wires, passes, wakes and counters; a helper's own thread-scope counters, such as `transport.notify.calls` for the puts it lands, are not kept. Nodes may differ; a node's PEs may not, in a job of more than one node. *Checked* among a node's PEs. |

**The transport's buffers**, which a program rarely needs to change

| Variable | Default | What it does |
|---|---|---|
| `OSOE_AM_CHUNK` | `65536` | The largest message on a connection: a power of two from `1024` to `1048576`; larger transfers are split, and a put's part is cut shorter, down to a sixteenth of a put message's largest payload (4,094 bytes at the default chunk), where it fills the room a buffer has left. *Checked* among a node's PEs in a job of more than one node, the refusal naming `OSOE_AM_*`. |
| `OSOE_AM_BUFFER` | four chunks, `262144` | Bytes of each connection's buffer: a power of two from twice `OSOE_AM_CHUNK` - the least allowed: a smaller value is refused, not raised - up to 1 GiB. The default is four chunks, twice the least. *Checked* among a node's PEs in a job of more than one node, as the buffer the budget leaves it, the refusal naming `OSOE_AM_*`. |
| `OSOE_AM_BUFFER_BUDGET` | `1073741824` | Bytes a node may spend on its connections' buffers together - two buffers for each other node. A decimal number of bytes, from twice `OSOE_AM_CHUNK` up to `68719476736` (64 GiB). Where the buffers do not fit, a buffer above its floor, twice the chunk, is halved until they do, and each node's lead says so once. At the defaults the 256 KiB buffers hold to 2,049 nodes, are halved to the floor from 2,050, and 4,097 nodes fit and 4,098 are refused: raise the budget or lower the chunk. With `OSOE_MESH_DATA_WIRES=on-demand` the buffers are chosen the same way, so on demand refuses nothing an eager job runs; past where even the floor holds every peer - 4,097 nodes at the defaults - it takes the floor and the budget counts the data wires a node makes instead of its peers, a buffer pair each, 4,096 a node at the defaults, each lead saying so once, and a data wire past the count ends the job naming the node, the count and both settings. The area keeps a place for every peer, which costs nothing until written, since a node writes only its wires' buffers. *Checked* among a node's PEs in a job of more than one node, as the buffer it leaves, the refusal naming `OSOE_AM_*`. |
| `OSOE_AM_CREDITS` | `64` | A connection's flow-control credits: `1` to `65536`. |
| `OSOE_TURNOVER_WAKE` | `room` | Which of the writers asleep waiting for room in a connection's buffer its reopening wakes. `room`, the default since 2026-10-29: as many as the reopened room holds messages of a chunk - four at the defaults - at once, a woken writer whose message was smaller than a chunk waking one more, so that a room of small messages wakes as many as fit. `all`, every one, each then trying for its share, as osoe did until then: when a node's PEs all send toward one other node at once, every writer sleeps at every turnover - a quarter of the node's PEs for each 64 KiB on the cluster's 16 to 256 PEs a node - each woken by the node's agent thread. `hand-on`, one, which wakes the next if its reservation leaves room for a put's smallest part - a sixteenth of a chunk - and so on along the room, a woken writer whose own message the room cannot hold, though a put's part would fit, passing its wake on, once a turnover. Each node's own: nodes may differ. `osoe-stat` counts the writers' wakes, `am.turnover_hand_ons` and `am.turnover_passes`. Timed in the development VM alone, the Limits say how. |
| `OSOE_AM_HUGEPAGES` | `0` | `1`: has no effect yet. It asks for huge pages with `MAP_HUGETLB`, which Linux refuses for the `/dev/shm` object the buffers live in, whatever huge pages are reserved, so each PE of a job of several nodes - which alone has the buffers - warns and maps them as before. (A `/dev/shm` mounted with `huge=always` gives them transparent huge pages either way.) |
| `OSOE_WIRE_BYTES`, `OSOE_WIRE_BUFFERS` | `16384`, `64` | The size and number of the buffers io_uring receives into, shared by the connections of each owner of the node's wires - the lead's agent, and each progress helper: the node holds their product an owner, 1 MiB at the defaults, however many peers it has. A size from `64` to `1048576` bytes; a count that is a power of two from `2` to `32768`. |
| `OSOE_WIRE_ZERO_COPY`, `OSOE_WIRE_ZERO_COPY_BYTES` | `off`, `16384` | `on` sends each connection buffer to the kernel by reference (`IORING_OP_SEND_ZC`) wherever a send carries at least `OSOE_WIRE_ZERO_COPY_BYTES`, and the buffer is not written again until the kernel has let go of it - over TCP, once the peer has acknowledged its bytes - so only one of a connection's two buffers goes by reference at a time, the other copying meanwhile; `off` copies, as before. Off by default, since it loses wherever a copy is cheap: over loopback the kernel copies anyway and adds a notification a send, and a stream ran at about 60% of the copying send's; below about 10 KB, by the kernel's own documentation, pinning the pages costs more than the copy; on the Pi cluster's gigabit a quarter of a stream's sends went by reference and the sending agent's CPU a byte was the copying send's - lending both buffers at once saved a third of it, but left a quiet waiting about half a second at a time for the peer's delayed TCP acknowledgement; and on a fast link a buffer lent is the kernel's until acknowledged, so a peer's stream sends at most one buffer by reference a round trip, the other turning over by copy meanwhile. On eight nodes of Google Cloud's 23 Gbit/s link (2026-10-20) it raised a stream 7 to 12% and took 6 to 9% off the progress threads' CPU where one thread a node held the rate back, and changed nothing where two helpers already reached the provider's ceiling; no send was copied after all and none waited behind its notification (`docs/fast-link-results.md`). So it stays off by default, with two buffers a connection (the author, 2026-10-20). Each send by reference is charged the length it offers, and two pages, until its notification comes, to one count of locked memory for every process of the user on a host, which io_uring holds to each process's own limit (`ulimit -l`; 8 MiB by default): a send the limit refuses goes again by copy, and the connection that met it copies from then on, which a notice naming the limit says once a process - sixteen logical nodes on an emulated gigabit link, each PE putting to every other, met the default, and until this fallback ended "Cannot allocate memory". Sixty-four logical nodes of one PE each on one host, every process held to the default 8 MiB, soft and hard, and every PE putting to every other, met refusals on every node in 20 runs of 20, and each job ended well with every byte right (`shmem.zero-copy-refusals.64-nodes`). `shmem_init` raises the soft limit to the hard one ([What `shmem_init` changes in the process](#what-shmem_init-changes-in-the-process)), so to keep sending by reference past that, the hard limit must be raised - an administrator's change, or Slurm's, whose steps on the Pi cluster have no hard limit. On Linux 6.14 and later io_uring charges its rings to the same count, zero copy or not, so a ring that cannot be made says the limit too. `OSOE_WIRE_ZERO_COPY_BYTES` from `1` up; with it on, a job of more than 3,274 nodes is refused at the defaults with no helpers - the completion queue of the ring that holds the data wires, which more `OSOE_WIRE_BUFFERS` fill sooner and helpers share out. `osoe-stat` counts the sends by reference, a refused one among them, their notifications, and those the kernel did not send by reference after all, a refused one's among them. Each node's own. |

**For testing**

| Variable | What it does |
|---|---|
| `OSOE_HEAP_SHIFT` | A size, a whole number of pages - `4k` is refused on a Raspberry Pi 5's 16 KiB pages: each PE maps its heap that far times its rank past the base, so no two PEs share a symmetric address - how a program is checked for assuming they do. |
| `OSOE_FORCE_REMOTE` | `1`: `shmem_ptr` and `shmem_team_ptr` answer NULL for node-mates, and `SHMEM_TEAM_SHARED` shrinks to the calling PE alone; the traffic to node-mates still goes through shared memory. Needs two logical nodes or more. |
| `OSOE_CHECK_COLLECTIVES` | `1`, the default; `0` turns it off. On, every collective's call is compared among the PEs of a node - its kind, operation, element, bytes, root or strides, as each PE says it where they meet - and a difference ends the job naming two PEs and their calls ("A collective that the PEs of one node call differently", Limits). Set it alike on every PE of a node: a PE that compares ends the job naming a node-mate that said no call, so a PE at `0` beside PEs at the default is named by the one that compares. On by default since 2026-10-28 (P39), off until then; timed at 16 to 256 PEs a node (H12, 2026-10-28), it cost nothing one round could see but a barrier's at 256, 672 µs to 975 in `osoe-bench`'s short run - mostly the first barriers' cost, each PE's first check of a team's sync reading a page of each node-mate's region, and about 3% of a barrier after a thousand in the development VM (Limits). |
| `OSOE_CHECK_QUIET` | `1`: every quiet on a context that threads share (`SHMEM_THREAD_MULTIPLE`) checks that it completed what it promises, and ends the job naming what it did not. |
| `OSOE_TEST_PERTURB` | `1`: short sleeps and yields at the internal choice points of a context that threads share, to widen races. |
| `OSOE_TEST_NOTICE_SECONDS` | Seconds before a start-up wait's first notice, `1` to `3600`, in place of 30, so that a test sees one without waiting. A testing hook, refused by name, as the settings above are, when it is not a whole number in that range. |
| `OSOE_TEST_DEADLOCK_DROPS` | A number of probes, `0` to `16`: a node other than the root drops that many of the deadlock detector's first probes, answering none, so that a test sees the root give the collection up after three seconds and begin again rather than wait for the job's life. Refused by name when it is not a whole number in that range. |
| `OSOE_TEST_ARCHITECTURE` | `other`: the node's lead claims, in the rendezvous, the architecture of the other of x86-64 and aarch64 - `long double`'s format the other's - so that a test sees a job of mixed architectures refused on one machine. Any other value is refused by name. |
| `OSOE_TEST_DEAF_HELPER` | A progress helper's number, `1` up to `OSOE_PROGRESS_HELPERS`: that helper never answers the lead's request to stop, so that a test sees the lead end the job at `shmem_finalize` naming it. Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_TEST_STALE_HELPER` | A progress helper's number, `1` up to `OSOE_PROGRESS_HELPERS`: that helper answers the deadlock detector's questions until its first yes and never again, so that a test sees a true deadlock go unproved - the detector takes an answer only to the question it asked last (`shmem.deadlock.helper-stale.0-0-1-1`). Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_TEST_BUSY_HELPER` | A progress helper's number, `1` up to `OSOE_PROGRESS_HELPERS`: that helper counts a message applied at every turn, as real traffic would, so that a test sees a true deadlock go unproved - the node's activity moves, and the detector counts every helper's applied messages in it (`shmem.deadlock.helper-busy.0-0-1-1`). Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_TEST_SLOW_HELPER` | A progress helper's number, `1` up to `OSOE_PROGRESS_HELPERS`: after each of the deadlock detector's questions that helper begins its pass only when the lead has asked again, so that its answer, which follows the pass, is to a question the lead has replaced, and a test sees a true deadlock go unproved - where an answer given before the pass would be taken (`shmem.deadlock.helper-slow.0-0-1-1`). Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_TEST_ONLINE_CPUS` | `1` to `65536`: the CPUs online that `OSOE_PROGRESS_HELPERS=auto` counts on each node, in place of the machine's, so that a test of the rule holds on any machine. Read, and checked, by every PE where the rule applies, the setting unset or `auto`. |
| `OSOE_TEST_HOLD_DATA_WIRES` | `NODE:MS`, with `OSOE_MESH_DATA_WIRES=on-demand`: node `NODE`'s lead dials and answers no data wire for `MS` milliseconds after start-up, `1` to `600000` - a data wire that stalls, so that a test sees its notice and its end under `OSOE_NO_PROGRESS_TIMEOUT` (`shmem.data-wires.stall.*`). Refused by name when it names no node of the job or no time. |
| `OSOE_TEST_HOLD_GOODBYE` | `FROM:TO`, two nodes of the job: node `FROM` holds its abortive goodbye to node `TO` half a second after its abort begins, so that a test sees `TO` hear a third node's goodbye first - the relay a global exit's kept stream is for; every other node ignores it. Refused by name when it is not two different nodes of the job. |
| `OSOE_TEST_HOLD_RELEASE` | `FROM:TO`, two nodes of the job: after node `FROM` queues a RELEASE of a team a split made toward node `TO`, its control wire to `TO` seals nothing for half a second, as a peer slow to read would keep it, so that a test sees the team's local release held until the RELEASE has left (`shmem.team-release.held.0-1-2`); every other node ignores it. Read, and refused by name when it is not two different nodes of the job, by each node's lead in a job of more than one node alone. |
| `OSOE_TEST_MESH_HOLD` | `NODE:MS`: node `NODE`'s lead reaches its mesh `MS` milliseconds late, `1` to `60000`, its callers waiting in its listener's backlog meanwhile - the schedule's late lead (`shmem.mesh.hold`). Refused by name when it names no node of the job or no time. |
| `OSOE_TEST_ABORT_DELAY_MS` | Milliseconds, `0` to `5000`: a node acts on a peer's abortive goodbye that late, and each progress helper begins its own abort that long after it sees the node's, so that a test sees what the lead's wait for its helpers' goodbyes is for. Read by each node's lead in a job of more than one node alone, and checked only there. |
| `OSOE_TEST_PRINT_LINK` | `1`: rank 0 prints the link it measured at the end of `shmem_init`, as the three `OSOE_LINK_*` settings that pin it, so that a test that failed on the shapes that link chose can be run again on them; nothing where the link is set by hand, the shape is `fixed` or the job has one node. Every test of more than one node in the suite sets it. Refused by name when it is not `0` or `1`. |
| `OSOE_TEST_TEAM_GENERATION` | A whole number: every team number a PE may create is taken as if that many times already, so that a test crosses a team handle's 2^32nd generation in a few splits (`shmem.team-split.generations`). Each PE's own; refused by name above the most generations a handle can carry, about 2^45 at the most team numbers. |
| `OSOE_WIRE_UNSENT_BYTES` | About the most the kernel may hold unsent on a connection's data wire, which its puts and gets go by (a control wire keeps the system's own): `0` leaves the system's own (`net.ipv4.tcp_notsent_lowat`, no limit by default); otherwise taken as given, however small, up to `1073741824`. Unset, the link's bandwidth times 128 µs, at least 16 KiB - 16 KiB on a gigabit - set at the end of `shmem_init` from the link, measured or set. `OSOE_COLLECTIVE_SHAPE=fixed` measures none, so there it is 16 KiB on any link unless the three `OSOE_LINK_*` give the bandwidth - less than the rule gives a link faster than a gigabit. Uncapped, as at v0.2 step 3, a fetch-add behind a put stream on the Pi cluster waited 17 to 20 ms; at 16 KiB, 3.3 to 3.9 ms, the stream at line rate either way; on Google Cloud's link at the default, 663 µs against 77 alone, 598 with two helpers (H16, 2026-10-28). |
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
| `OSOE_MACHINE_PES` | Optional: the job's PEs on this machine, a number from `1` to the job's PEs, which the progress helpers' rule (`OSOE_PROGRESS_HELPERS=auto`, the default) counts against the machine's CPUs online in place of a node's PEs when it is more; `osoe-launch` sets it to every rank, its logical nodes sharing one machine. Unset elsewhere, where a node is a machine. Read, and refused by name when it is not such a number, by every PE where the rule applies, the helpers' setting unset or `auto` |

- The job id and step id name the job's shared memory
  (`/dev/shm/osoe.<job>.<step>.…`). Two starts with the same pair at once
  collide, and the second fails at init; a job past its start holds no names
  (below), so a later job may use the pair again.
- `OSOE_GTIDS[OSOE_LOCALID]` must equal `OSOE_PROCID`.
- `mpirun` sets none of them, and a program it starts is refused at
  `shmem_init`: "MPI in the same program", under Limits, says how to start one
  under it.

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
same ids reclaims it or the epilog below removes it. Nor does it judge a
control segment made in another PID namespace than its own, whose pids name
other processes here: it leaves one alone, saying so (Building). `osoe-stat --gc` works on the
machine it runs on and removes only what its user may, so a site runs it as
root on each node - from a timer, not from Slurm's epilog: it reads objects
any user can write. It reads each into its own memory, so that one changed or
cut short as it reads cannot fault it, leaves alone one it cannot read, and
counts in its status only what it failed to do - a removal refused, a name it
was given that is not there - where one file of garbage made every run fail,
which in an epilog drains the node; but what it reads is still any user's to
write, and the epilog's safe line is the `rm` below.
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
on it, and they end by themselves if its process ends after it has made the
node's segment - or, in a start without a launch number, after it has finished
it: the lead
names the local PEs that have not joined, and their ranks; a node's first other
PE, while its lead has made no segment, names the lead; a lead calling another
node names that node, its host-list entry, the address and ports it dialled and
the last error ("Connection refused", "the connection timed out", "no answer to
the hello"); a lead waiting to be called names the nodes it waits for, and a
lead connecting its mesh, calling and called at once, says both in its one line;
and a lead whose progress thread, created, the system has not yet run says so.
The launcher ends what remains - `srun -K1` at the first task that fails - and
`OSOE_NO_PROGRESS_TIMEOUT` ends a wait that stops moving. What remains includes
the node-mates of a lead that dies before it has made the segment and, in a
start without a launch number, before it has finished it, which is chiefly the
creating of its heap. The number is how a PE tells the segment of its own start
from a failed start's leftover by the same names, which its lead is about to
replace; without one it does not take the process id in the segment for its
lead's. `osoe-launch` gives every start a number, `OSOE_LAUNCH` gives one by
hand, and a requeued Slurm job has `SLURM_RESTART_COUNT`; the first run of a
Slurm job has none. The start's last step, once every node is connected - the
link's measurement and the reduction that hands every PE its numbers - waits as
a collective does: a hang there says nothing at 30 s, and shows in `osoe-stat`
as a collective's would, its routine `shmem_init`.

**On demand** (`OSOE_MESH_DATA_WIRES=on-demand`), a pair's data wire is not
part of the start: the mesh is each pair's control wire, and the data wire is
made the first time a PE of either node has something for the other, the PE
waiting meanwhile in the routine that needed it. For the control wires the
start promises what it does eagerly: every node is reached in `shmem_init`, or
the job ends saying which was not. A data wire that cannot be made is met where
it is first needed - which for the collective tree's wires is still the end of
`shmem_init`, whose last step uses them - and is told as the start's calls are:
after 30 s and every minute, the lead dialling names the node, its address and
the last error, and a lead that asked another to dial names the node it waits
for. `OSOE_NO_PROGRESS_TIMEOUT` ends such a wait, and the deadlock detector
never takes one for a deadlock.

**Trust.** osoe assumes a private cluster network, as Slurm does. With
`OSOE_JOB_SECRET`, a host that does not know the secret cannot join the job:
every lead proves it before it is counted, and nothing a caller claims is acted
on before it has. The secret does not protect the job's start from a host that
can reach the ports and holds connections open - a lead serves sixteen callers
at once, and a full set leaves the rest waiting - nor from a process on a lead's
host that holds a port the lead would answer on and answers first, which a
caller takes for a job launched with another secret and ends; nor the traffic
after the handshake, which is not encrypted or authenticated. Without the
secret, any host that can reach the ports can join - and a host that joins is a
node: it can read and write every PE's symmetric memory, the program's global
and static variables among them.

### What `shmem_init` changes in the process

`shmem_init` raises two of the process's resource limits, each from its soft
limit to its hard one, before it opens anything either counts, and changes no
other:

- **Open files** (`RLIMIT_NOFILE`, `ulimit -n`). A node's lead holds two
  sockets for every other node - its control wire and its data wire - and a
  descriptor for every other PE of its node, beside the program's own. Slurm
  passes on the submitting shell's limits, often a soft 1,024, which at 256 PEs
  a node runs out at about 385 nodes.
- **Locked memory** (`RLIMIT_MEMLOCK`, `ulimit -l`). On Linux 6.14 and later
  io_uring charges every ring to one count of locked memory for all the user's
  processes on a host, held to each process's own soft limit, and on every
  kernel the zero-copy send (`OSOE_WIRE_ZERO_COPY`) charges the pages it lends
  to the same count. A node's own rings take little - at most 2.6 MiB a lead at
  the 4,097-node ceiling, on a Pi's 16 KiB pages at up to three helpers - but
  one machine's logical nodes count together: with
  the 8 MiB soft limit a Raspberry Pi's shell passed to its Slurm step, the
  release's tier-2 tests, four at once on one node, failed beside each other,
  and with the soft limit raised in the step, whose hard limit is unlimited,
  they passed (`docs/reviews/archive/v0.2-before-part4-review.md`, P9-1).
  Raised since 2026-10-11, by the author's decision
  (`docs/decisions/2026-10-11.md`).

Neither can make a program that ran before fail: only a descriptor it could
not have had, or memory it could not have locked, lands above the old limit.
The locked memory is counted for the user on the host, though, so a job that
now locks more than its old limit - the zero-copy send's lent pages, under a
generous hard limit - leaves less of the count to the user's other processes
there, each held to its own limit.
Each PE raises its own, and the limits stay raised after `shmem_finalize`; a
process the program starts inherits them, as it inherits every limit. The hard
limits are the administrator's: where the hard limit is the soft one - a
desktop's 8 MiB of locked memory, often - nothing is raised, and a ring the
limit refuses ends the job at start-up, naming the limit. Under Slurm a step's
hard limits are `slurmd`'s, unlimited on the Pi cluster, where the soft ones
come from the submitting shell.

### How a job ends

A PE that dies, or a node that vanishes, ends the whole job with a failure
(design §3.15); a node whose PEs do not share a PID namespace is refused as it starts (Building). `shmem_global_exit(status)` ends every PE, and the job with
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
theirs, as §9.1.6 asks: on each node the first call wins, and of the nodes'
winners the lowest-ranked carries the job and leaves last of it; every other
caller leaves with 0, after its handlers if the library saw its call before
the job's end reached its PE. So PEs of every node calling at once, as they
all do when each detects the same failure, still end the job with one bad
exit, the last, and `-K1` kills nobody: a node whose winner learns of a lower
one from that node's goodbye leaves with 0 everywhere before the job's carrier
leaves (design §3.15). A caller whose call the job's end outran - another
node's goodbye, or another caller's declaration on its own node - leaves as a
PE that did not call, its streams flushed and its handlers not run. When every
PE calls at once that can be many of them - P1's test of every PE calling,
which first required every handler, failed on exactly that
(`docs/decisions/2026-10-29-exit-carrier.md`) - so a program whose handlers
must run does that work before it calls.

One limit remains. A site that sets `WaitTime`, or `srun -W n`, ends the
other tasks n seconds after the first one leaves, whatever its status.
A job proved deadlocked ends the same way as a failure, with status 1, after
each node has printed where its PEs wait.

### Further reading

- [Where to find what](docs/README.md) - the index of `docs/`: using osoe, how
  it works, and the record of how it was built
- [Design notes](docs/design/shmem-ethernet-design.md) - every protocol and data
  structure, and why
- [The Pi cluster's results](docs/pi-cluster-results.md), and its
  [runbook](docs/pi-cluster.md)
- [The fast link's results](docs/fast-link-results.md), and its
  [runbook](docs/fast-link.md)
- [Release notes](docs/release-notes/) - what each release is, what changed, and
  what was checked before its tag
- [The reviews](docs/reviews/README.md) - the adversarial reviews of the design
  and of the tree, and how one is run
- [Decisions in force](docs/decisions/README.md), each linked to the dated
  record that explains it
- [Specification reference](docs/reference/README.md)

## How osoe was made

The scope and goals of osoe were first defined by the author and then Claude
Code, Anthropic's agentic coding tool, was used to consider the goals and the
OpenSHMEM specification to generate plans, carry out implementation, and perform
testing on an Arm VM running on macOS and a Raspberry Pi 5 cluster running
Raspberry Pi OS (Trixie), and in three sessions on eight x86-64 nodes of Google
Cloud. The work was carried out starting in September 2026.
The models were the ones Anthropic offered during September/October 2026:
Claude Opus 5, and from 22 September Claude Opus 5.5, led each working session;
Claude Sonnet 5 and 5.5 took parts of some sessions and of the tasks a session
handed off, and Claude Fable 5.1 did periodic reviews and deep planning. Every
commit names the model that wrote it beside the author, in a `Co-Authored-By`
line, but the first, eleven merges, fourteen commits of the overnight batches
of 19 and 20 October, and one work-in-progress commit, `de1e2f8`,
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
  nine points - rounds two to ten,
  round one having read the design alone - four or five more reviewed the whole tree:
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
