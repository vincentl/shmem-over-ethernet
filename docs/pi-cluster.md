# Running osoe on the Raspberry Pi cluster

The runbook for the first experiments on real hardware: four Raspberry Pi 5
compute nodes, `pi-001` to `pi-004` (16 GB each), booting over the network from
a head node (8 GB, 512 GB NVMe), all on one gigabit PoE switch, with Slurm as
the launcher. Every step names what it checks and what to keep. Written for
Raspberry Pi OS Trixie (Debian 13); the first step says whether that holds.

The emulated version of this cluster is tier 3's `pi` link (1 Gbit/s, 100 µs
each way; design §8.2): there the library's round trips measured 1.01–1.06×
an io_uring echo's and its streams ran at line rate, about 114 MiB/s. That was
on a fast CPU. The question these experiments answer first is what the Pi 5's
Cortex-A76 cores do to it.

## 1. The nodes

Each compute node must have Linux 6.7 or newer and io_uring enabled:

```sh
srun -N4 --ntasks-per-node=1 uname -r
srun -N4 --ntasks-per-node=1 cat /proc/sys/kernel/io_uring_disabled   # 0
sinfo -V
```

Keep the output: it goes in the notes with the results. Trixie gives kernel 6.12
and Slurm 24.11; Bookworm's may be too old for osoe (6.6), which `probe` below
says plainly.

## 2. Build once, on the shared filesystem

The compute nodes have no disks, so the tree lives on the filesystem every
node mounts; build it once, at the same path everywhere:

```sh
sudo apt install build-essential cmake pkg-config liburing-dev git
cmake --preset release
cmake --build build/release -j4
srun -N4 --ntasks-per-node=1 build/release/tools/probe/probe     # every node: all ok
```

`probe` checks every io_uring and futex facility osoe relies on. If it fails on
a node, nothing else will work there.

The test suite runs on one node, inside a step of one task given all four
cores - a task gets one by default, and every process the tests start would
share it:

```sh
srun -N1 -n1 -c4 ctest --test-dir build/release -L 'tier1|tier2' -j4 --timeout 300
```

The floor pins (`shmem.floor`) and `echo-uring`'s were set on the VM's CPU and
may fail on a Pi. That is a finding to keep, not a reason to stop.

## 3. Slurm's environment, captured

The bootstrap's Slurm path (design §6.1) has been run on one node with a real
`srun`, and across nodes only through `osoe-launch -s`, which names the
variables as `srun` does. Capture the real multi-node environments first; they
become tier-1 tests:

```sh
F=shmem/tests/slurm-fixtures
tools/launch/capture-slurm-fixtures.sh block-4x4  $F -N4 --ntasks-per-node=4
tools/launch/capture-slurm-fixtures.sh cyclic-4x4 $F -N4 --ntasks-per-node=4 --distribution=cyclic
tools/launch/capture-slurm-fixtures.sh plane-4x4  $F -N4 --ntasks-per-node=4 --distribution=plane=2
tools/launch/capture-slurm-fixtures.sh uneven     $F -N3 -n7
salloc -N4 sh -c "tools/launch/capture-slurm-fixtures.sh relative $F -N2 -r2 -n2"
tools/launch/capture-slurm-fixtures.sh exclude    $F -N3 -n3 -x pi-002
ctest --test-dir build/release -R 'shmem\.bootstrap\.' --output-on-failure
```

Every `captured-*` fixture must pass. One that fails is either the bootstrap
misreading Slurm or §6.1 describing it wrongly; which one is the finding.

## 4. The smoke test

```sh
srun -N4 --ntasks-per-node=4 build/release/shmem/tests/shmem_test_internode
srun -N4 --ntasks-per-node=4 --distribution=cyclic build/release/shmem/tests/shmem_test_internode
```

Both must exit 0 on every task. The cyclic one is the layout the environment
cannot describe and the rendezvous must assemble (design §6.1).

## 5. The library against its floor

Two nodes, one PE on each: the library's get, put and quiet, fetch-add and
stream, against an io_uring echo and stream between the same two processes on
the same link, in seven interleaved pairs (design §8.4):

```sh
srun -N2 --ntasks-per-node=1 build/release/shmem/tests/shmem_test_floor > floor.txt
```

Keep `floor.txt`. Its `ratio` lines are the numbers to compare with tier 3's
`pi` link: fastest trips at 1.01–1.06× the floor there, and streams at line
rate.

## 6. The benchmark

`osoe-bench` prints one measurement a line: put and quiet and get latency by
size, stream bandwidth, fetch-add latency, barrier time, and every PE
streaming at once (`tools/bench/osoe-bench.c`).

```sh
B=build/release/tools/bench/osoe-bench
srun -N2 --ntasks-per-node=1 $B > bench-2x1.txt
srun -N4 --ntasks-per-node=1 $B > bench-4x1.txt
srun -N4 --ntasks-per-node=4 $B > bench-4x4.txt
srun -N4 --ntasks-per-node=4 --cpu-bind=none $B > bench-4x4-unbound.txt
```

The last pair is the experiment the design most wants from four-core nodes.
Each PE has an agent thread beside its main thread (§3.12), and Slurm's
default binding may pin both to one core. The difference between the two runs
is what that costs.

## 7. Endings

A global exit from one PE, while every other PE computes, must end every task
with the caller's status. A killed PE must end the job with a failure:

```sh
srun -N4 --ntasks-per-node=4 build/release/shmem/tests/shmem_test_global-exit 7 1 compute; echo $?
srun -N4 --ntasks-per-node=4 -K0 build/release/shmem/tests/shmem_test_dead-pe 5; echo $?
```

The first must print 7, and every task's "PE n: computing" line. The second
must end every task within a few seconds, with the line naming PE 5 as dead.

A vanished node needs care here: these nodes boot from the network, so a node
whose cable is pulled loses its root filesystem too, and will need its PoE port
power-cycled afterwards. From the others' side it is still the test: keepalive
should end the job one to two minutes after the cut (design §3.15).

## 8. What to bring back

- the outputs of steps 1, 5, 6 and 7;
- the fixtures step 3 captured, to commit with the tests they feed;
- anything that failed, with its output, and the node it failed on.
