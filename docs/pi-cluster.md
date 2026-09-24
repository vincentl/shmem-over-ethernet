# Running osoe on the Raspberry Pi cluster

The runbook for the first experiments on real hardware: four Raspberry Pi 5
compute nodes, `pi-001` to `pi-004` (16 GB each, at 10.0.1.1 to 10.0.1.4),
booting over the network from the head node `pi-login`, all on one gigabit PoE
switch, with Slurm as the launcher. The cluster is `pi-cluster`'s: every
compute node shares one read-only root, the image on the head node, and mounts
`/home` read-write from it, so a checkout in `/home` is at the same path on
every node, and software reaches the nodes only through the image
(`pi-cluster/head/image-run.sh`). Slurm is set up by hand there for now.

The emulated version of this cluster is tier 3's `pi` link (1 Gbit/s, 100 µs
each way; design §8.2): there the library's round trips measured 1.01–1.06×
an io_uring echo's and its streams ran at line rate, about 114 MiB/s. That was
on a fast CPU. The question these experiments answer first is what the Pi 5's
Cortex-A76 cores do to it.

## 1. The nodes

```sh
srun -N4 --ntasks-per-node=1 uname -r
srun -N4 --ntasks-per-node=1 getconf PAGESIZE                          # 16384 on a Pi 5's kernel
srun -N4 --ntasks-per-node=1 cat /proc/sys/kernel/io_uring_disabled   # 0
srun -N4 --ntasks-per-node=1 cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname): $(getent ahostsv4 $(hostname) | head -1)"'
sinfo -V
```

Keep the output: it goes in the notes with the results.
- osoe needs Linux 6.7 or newer, and `probe` below checks the rest.
- A Pi 5's kernel uses 16 KiB pages where the VM's uses 4 KiB. osoe sizes every mapping by the page it is given (design §8.5), and this is the first run that proves it. `OSOE_HEAP_BASE`, if you set it, must be a multiple of that page.
- Memory: a node's four PEs hold about 1 GiB of `/dev/shm` at the default heap, 256 MiB a PE, touched only as it is used, and under 2 MiB besides. A 16 GB node's `/dev/shm` is 8 GB by default.
- The governor matters to every time below: `ondemand` clocks a core down between trips, so write down which one ran.
- The fifth line shows what each node's own name resolves to there. On this cluster the image's `/etc/hosts` maps each name to its 10.0.1.N address. A stock Raspberry Pi OS maps a host's own name to 127.0.1.1, which its peers cannot reach; osoe then listens at the node's address toward the others instead (design §6.1), and `SHMEM_DEBUG=1` says so per node.

## 2. Build once, in /home

On the head node, the tools; into the compute nodes' image, with the nodes
powered off, the library osoe loads at run time and `ctest`:

```sh
sudo apt install build-essential cmake pkg-config liburing-dev git
sudo bash ~/pi-cluster/head/image-run.sh apt-get install -y liburing2 cmake
```

Then, in the checkout under `/home`, on the head node:

```sh
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
srun -N1 -n1 -c4 ctest --test-dir build/release -L 'tier1|tier2' -E '^model\.' -j4 --timeout 300
```

The models (`model.*`) are left out: they check designs, not this machine,
and take the better part of an hour on a Pi. The floor pins (`shmem.floor`) and
`echo-uring`'s were set on the VM's CPU and may fail on a Pi. That is a finding
to keep, not a reason to stop.

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
cmake --build build/release                      # registers the new fixtures
ctest --test-dir build/release -N -R 'shmem\.bootstrap\.captured'
ctest --test-dir build/release -R 'shmem\.bootstrap\.' --output-on-failure
```

The listing must show the new `captured-*` names, and every one must pass. One
that fails is either the bootstrap misreading Slurm or §6.1 describing it
wrongly; which one is the finding.

## 4. The smoke test

For the first runs, a wait that stops moving should say what it waits for, and
a task that fails at start-up should end the rest (`-K1`):

```sh
export OSOE_NO_PROGRESS_TIMEOUT=60
T="env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2 OSOE_WIRE_BYTES=256 \
     OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919"
srun -K1 -N4 --ntasks-per-node=4 $T build/release/shmem/tests/shmem_test_internode
srun -K1 -N4 --ntasks-per-node=4 --distribution=cyclic $T build/release/shmem/tests/shmem_test_internode
```

Both must exit 0 on every task. The settings are the test's own (its ctest
registration): small segments and two reply credits, so that segment splitting
and the credit limits cross the real link. The cyclic run is the layout the
environment cannot describe and the rendezvous must assemble (design §6.1).

## 5. The library against its floor

Two nodes, one PE on each: the library's get, put and quiet, fetch-add and
stream, against an io_uring echo and stream between the same two processes on
the same link, in seven interleaved pairs (design §8.4). `-c4` gives the one
task all four cores: with one, its main thread, its agent and the floor's echo
would share a core, and that cost would read as the Pi's:

```sh
srun -K1 -N2 --ntasks-per-node=1 -c4 build/release/shmem/tests/shmem_test_floor > floor.txt
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
srun -K1 -N2 --ntasks-per-node=1 -c4 $B > bench-2x1.txt
srun -K1 -N4 --ntasks-per-node=1 -c4 $B > bench-4x1.txt
srun -K1 -N4 --ntasks-per-node=4 $B > bench-4x4.txt
srun -K1 -N4 --ntasks-per-node=4 --cpu-bind=none $B > bench-4x4-unbound.txt
```

The last pair is the experiment the design most wants from four-core nodes.
Each PE has an agent thread beside its main thread (§3.12), and Slurm's default
binding gives each of the four tasks one core, which its two threads share;
unbound, they may use any of the node's four. The difference between the two
runs is what the sharing costs.

## 7. Endings

A global exit from one PE, while every other PE computes, must end every task
with the caller's status. A killed PE must end the job with a failure. Here
`-K0`: what ends the tasks must be the library, not Slurm:

```sh
srun -K0 -N4 --ntasks-per-node=4 build/release/shmem/tests/shmem_test_global-exit 7 1 compute; echo $?
srun -K0 -N4 --ntasks-per-node=4 build/release/shmem/tests/shmem_test_dead-pe 5; echo $?
```

The first must print 7, and every task's "PE n: computing" line. The second
must end every task within a few seconds, naming rank 5 as dead.

A vanished node needs care here: these nodes boot from the network, so a node
whose cable is pulled loses its root filesystem too, and will need its PoE port
power-cycled afterwards. From the others' side it is still the test. With the
connections idle, keepalive ends the job one to two minutes after the cut; with
data in flight toward the lost node, the kernel's retransmission limit
(`tcp_retries2`) decides instead, about fifteen minutes (design §3.15).

## 8. What to bring back

- the outputs of steps 1, 5, 6 and 7;
- the fixtures step 3 captured, to commit with the tests they feed;
- anything that failed, with its output, and the node it failed on.
