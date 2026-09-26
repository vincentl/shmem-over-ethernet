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
- Memory: a node's four PEs hold about 1 GiB of `/dev/shm` at the default heap, 256 MiB a PE, touched only as it is used, and about 22 MiB besides: each PE's internal region, 5 MiB since phase 1 step 5 slice 5 gave every team number's block of it (32 by default, since phase 1 step 7) room for two pieces in flight per child (`OSOE_COLLECTIVE_PIPELINE`) - 20 MiB for the four, touched only by a team's leads - the node's control segment, 1.3 MiB, and its connection buffers, 0.8 MiB (the last two measured on the VM's 4-node, 4-PE map; the region's size is computed, and was 2.5 MiB a PE before the slice). A 16 GB node's `/dev/shm` is 8 GB by default.
- The governor matters to every time below: `ondemand` clocks a core down between trips, so write down which one ran. Both sessions so far ran `ondemand`.
- The switch: compute node `pi-00X` is on port X and the head node on port 9; the switch is unmanaged as far as known, so it has no counters to read. What TCP saw of it is in `nstat -az TcpRetransSegs TcpExtTCPTimeouts` on each node.
- The fifth line shows what each node's own name resolves to there. On this cluster the image's `/etc/hosts` maps each name to its 10.0.1.N address. A stock Raspberry Pi OS maps a host's own name to 127.0.1.1, which its peers cannot reach; osoe then listens at the node's address toward the others instead (design §6.1), and `SHMEM_DEBUG=1` says so per node.

## 2. Build once, in /home

Everything is compiled on the head node, which has the tools. The compute
nodes' image gets, with the nodes powered off, only the library every osoe
program loads at run time, and `cmake` for its `ctest`:

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

The test suite runs in two halves. Tier 1 is the build's own checks - the
headers under every standard, a static link, a strict-C11 rebuild, the exported
symbols, `oshcc` - which run the compiler and need `liburing-dev`, so they run
on the head node, beside the build. Tier 2 starts the library's processes, and
runs on a compute node, inside a step of one task given all four cores - a task
gets one by default, and every process the tests start would share it:

```sh
ctest --test-dir build/release -L tier1 -j4 --timeout 300
srun -N1 -n1 -c4 ctest --test-dir build/release -L tier2 -E '^model\.' -j4 --timeout 300
```

A compute node therefore needs no compiler and no headers.

The models (`model.*`) are left out: they check designs, not this machine,
and take the better part of an hour on a Pi. Tier 1's label carries three of
them too; on the head node add `-E '^model\.'` there as well, or budget 25
minutes for them (the second session ran them: 732 s for `tree_barrier`). The floor pins (`shmem.floor`) and
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
cmake --build build/release    # registers the new fixtures: "GLOB mismatch!" is CMake noticing them
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
size, stream bandwidth, fetch-add latency, barrier time, a reduction's and a
broadcast's time at 8 bytes and 64 KiB, an fcollect's and an alltoall's at a
block of 8 bytes and of 4 KiB from every PE, and every PE streaming at once
(`tools/bench/osoe-bench.c`). The fcollect of 8 bytes is ISx's end-of-run
gather; its generic internode phase was the one whose latency grew faster
than log N until phase 1 step 5 slice 6 made it an allgather of ceil(log2 N)
steps (design §5.5, §5.8), so its line at 16 PEs is the one to compare with
the first session's.

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

## 8. Programs from the corpus

Built with nothing but `oshcc`, as a user would build them, in copies under
`/home` - never in `corpus/` itself. `oshcc` is the build tree's
(`build/release/tools/wrapper/oshcc`); it makes each program's global and
static variables symmetric (design §4.3), which most of these need.

```sh
W=~/osoe-corpus; OSHCC=$PWD/build/release/tools/wrapper/oshcc
rm -rf $W && mkdir -p $W
cp -R corpus/openshmem-npbs/C $W/npb
cp -R corpus/prk $W/prk && : > $W/prk/common/make.defs
cp -R corpus/openshmem-examples/c $W/examples
```

NPB IS, class A on sixteen PEs. Its make takes the PE count at build time;
`is.c` includes `mpi.h` without calling anything in it, so an empty one stands
in; and it is C of the 1990s - `main` with no return type - which GCC 14
refuses unless `-fpermissive` makes that a warning again:

```sh
cd $W/npb && mkdir -p stub && : > stub/mpi.h
sed -e "s|^SHMEMCC.*|SHMEMCC = $OSHCC|" -e 's|^CFLAGS.*|CFLAGS = -O2 -g -fpermissive|' \
    -e 's|^CLINKFLAGS.*|CLINKFLAGS = -O2 -g|' -e "s|^CMPI_INC.*|CMPI_INC = -I$W/npb/stub|" \
    config/make.def.template > config/make.def
make is CLASS=A NPROCS=16
srun -K1 -N4 --ntasks-per-node=4 bin/is.A.16 | tee ~/is-A-16.txt   # "Verification = SUCCESSFUL"
make is CLASS=B NPROCS=16
srun -K1 -N4 --ntasks-per-node=4 bin/is.B.16 | tee ~/is-B-16.txt
```

Its timer counts whole seconds (`common/c_timers.c` returns `tv_sec`), so a
class that finishes in under one reports 0 seconds and "inf" Mop/s; class B is
the one to time.

PRK's stencil, synch_p2p and transpose, each on 4 × 4:

```sh
cd $W/prk/SHMEM
for k in Stencil:stencil Synch_p2p:p2p Transpose:transpose; do
  (cd ${k%%:*} && make SHMEMCC="$OSHCC -std=gnu11" DEFAULT_OPT_FLAGS=-O2 LIBS=-lm ${k##*:})
done
srun -K1 -N4 --ntasks-per-node=4 Stencil/stencil 20 2000 | tee ~/prk-stencil.txt
srun -K1 -N4 --ntasks-per-node=4 Synch_p2p/p2p 10 2000 2000 | tee ~/prk-p2p.txt
srun -K1 -N4 --ntasks-per-node=4 Transpose/transpose 10 2048 | tee ~/prk-transpose.txt
```

Each must print "Solution validates". ISx, weak and strong scaling, on 4 × 4.
It allocates a 1 GiB symmetric key buffer on every PE, so the heap must be
larger than osoe's default, and it asks for `-std=c99`, under which glibc's
`time.h` has no `struct timespec`:

```sh
cp -R corpus/isx/SHMEM $W/isx && cd $W/isx
make CC=$OSHCC CFLAGS="-O2 -std=gnu99 -Wall"
export SHMEM_SYMMETRIC_SIZE=1200M
srun -K1 -N4 --ntasks-per-node=4 bin/isx.weak 16777216 ~/isx-weak.log | tee ~/isx-weak.txt
srun -K1 -N4 --ntasks-per-node=4 bin/isx.strong 268435456 ~/isx-strong.log | tee ~/isx-strong.txt
unset SHMEM_SYMMETRIC_SIZE
```

A node's four PEs then hold 4.8 GiB of `/dev/shm` at most, under the 8 GB a
16 GB node has, though only what ISx touches is ever allocated. ISx verifies
its own sort and exits nonzero if it failed. And the examples, each on 4 × 2,
one line of verdict per program:

```sh
cd $W/examples
for src in *.c; do
  p=${src%.c}
  $OSHCC -std=gnu11 -O1 $src -o $p -lm 2> $p.build || { echo "$p: build failed"; continue; }
  timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./$p > $p.out 2>&1 < /dev/null
  echo "$p: exit $?"
done | tee ~/examples.txt
```

The same programs ran under `osoe-launch` in the development VM, four PEs on
two logical nodes, on 2026-09-24. **What to expect** from that run:
- PRK's three kernels validate.
- Of the 72 examples, all but the following exit 0.
  - `bar_pair` and `bcast4` pass an active set smaller than the job, which osoe
    refuses by name (design §7.4). So, on more than four PEs, do `bcast3`,
    `collect32`, `reduce-max` and `reduce-sum`, whose active set is PEs 0 to 3
    written out - the whole job on the VM's four, half of it on the cluster's
    4 × 2 (2026-09-24, and in the VM on 8); all but `collect32` also call with
    PEs outside it, which the spec leaves undefined anyway.
  - `nullput` puts to a null pointer. `just-a-shmalloc` and `shmem_2dheat`
    exit without `shmem_finalize`, which only a `start_pes` program may do
    (the latter calls `exit` on the line before its finalize). `addr-accessible`
    reports its own stack check as a failure: its test is inverted. osoe
    names each of the first three as such.
  - `global_exit` exits 1 where the program asks for 99: `osoe-launch` reports
    the job's failure, not the status (§7 above checks the status under
    `srun`, and on the cluster it exits 99). It calls `shmem_global_exit`
    straight after `shmem_init`, and can leave the job's names in `/dev/shm`
    behind it - the window design §6.2 accepts, which `osoe-stat --gc` clears.
- NPB IS verifies, classes S and A, on four PEs over two logical nodes and
  over four.
- ISx, weak scaling on 0,0,1,1 and strong on 0,1,2,3, sorts and verifies,
  with `SHMEM_SYMMETRIC_SIZE=1200M` - without it `shmem_malloc` returns NULL
  for the key buffer, which ISx does not check, and every PE dies of a
  segmentation fault.

A program that passed there and fails here is the finding.

### The OSU micro-benchmarks

OSU's OpenSHMEM benchmarks (`corpus/osu-omb`, 7.5.2) are the numbers other
implementations publish. Their own `configure` finds OpenSHMEM by linking with
`CC`, and finds no MPI, so it builds the OpenSHMEM half alone. A copied tree
leaves `configure.ac` newer than `configure`, and `make` would then rerun
autoconf, which the cluster need not have; the stamps go back in order first.
`configure` also bakes the compiler's absolute path into every Makefile, so a
checkout that moves needs `make distclean` and a new `configure` (the second
session found the first session's nested path there):

```sh
cp -R corpus/osu-omb/osu-micro-benchmarks-7.5.2 $W/osu && cd $W/osu
./configure CC=$OSHCC CXX=g++
find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) \
     -exec touch -t 202001010000 {} +
find . -name configure -exec touch {} +
find . -name Makefile.in -exec touch {} +
find . -name Makefile -exec touch {} +
make -C c/openshmem -j4 && cd c/openshmem
for p in put get put_bw get_bw put_nb get_nb put_nb_bw get_nb_bw put_overlap \
         get_overlap atomics put_mr put_mr_nb get_mr_nb; do
  srun -K1 -N2 --ntasks-per-node=1 ./osu_oshm_$p heap > ~/osu-$p.txt
done
for p in barrier broadcast collect fcollect reduce; do
  srun -K1 -N4 --ntasks-per-node=1 ./osu_oshm_$p -m :1048576 > ~/osu-$p-4x1.txt
  srun -K1 -N4 --ntasks-per-node=4 ./osu_oshm_$p -m :1048576 > ~/osu-$p-4x4.txt
done
```

The fourteen point-to-point programs measure a pair, one PE per node here.
Ten of them exit before `shmem_finalize` when given other than two PEs, which
osoe names and ends the job for, as it does `just-a-shmalloc`'s; the other four
run PEs 0 and 1 alone whatever the job. **`put_bw`, and the non-fetching rows
of `atomics`, stop the clock after their loop of calls and before any quiet**:
they measure how fast osoe takes the calls, into about 3 MB of send buffering,
not how fast the bytes arrive - on the cluster, 3.5 GB/s at 16 KiB. The rest
quiet inside the clock: `put_nb_bw` after every put, `put_mr` and `put_mr_nb`
once at the end of their batch. The development VM (2026-09-24) built all
nineteen, and each printed its whole table on two PEs over two logical nodes,
and the five collectives on four.

### osb's ssca1

Smith-Waterman over sequences spread across the PEs (`corpus/osb/ssca1`), whose
verification run inserts six known motifs and must find them. **Its data
generator races**: rank 0 puts the motifs into the other PEs' shares while
those PEs are still filling them with random codons, with no barrier between,
so a motif survives or not by timing - one, two or three alignments from the
same seed, on one node or four, in the VM. One barrier after the fill makes it
deterministic:

```sh
cp -R corpus/osb/ssca1 $W/ssca1 && cd $W/ssca1
sed -i '/sequence->sequence\[idx\] = rand()%64;/{n;s/^  }$/  }\n  BARRIER_ALL();/}' gen_scal_data.c
make COMPILER=oshcc CC=$OSHCC
SCALE=14 srun -K1 -N2 --ntasks-per-node=2 ./ssca1 | tee ~/ssca1-2x2.txt
SCALE=14 srun -K1 -N4 --ntasks-per-node=1 ./ssca1 | tee ~/ssca1-4x1.txt
```

Both must print "Found 3 acceptable alignments with scores from 55 to 53" and
three "succeeded" lines: with the barrier the VM found exactly that in each of
25 runs across three maps, including alignments that cross PEs' boundaries,
and without it the counts varied from run to run on every map.

The rest of osb is not worth the cluster's time. `nas/C`'s IS is NPB's IS
above. `GUPs` builds (with `-D_GNU_SOURCE`) but has no knob for its table size
and takes minutes a run at loopback latency. `shoms` needs its hardcoded x86
timer changed and then calls `shmem_finalize` in the middle of its sweep, which
osoe ends the job for once 126 of its 128 tests have run. `Graph500`'s own
SHMEM shim declares `shmem_int_alltoall` and `shmem_long_alltoall`, names
OpenSHMEM 1.5 has since given its team collectives, so it does not compile
against any current implementation. And the `_threaded` variants need
`SHMEM_THREAD_MULTIPLE` (design §4.11): `ssca1_threaded` asks for it, does not
check what it got, and calls SHMEM from several OpenMP threads at once, which
osoe, providing `SERIALIZED`, catches as two gets' replies crossing and ends the
job for.

### The SOS test suite

Sandia OpenSHMEM's own tests (`corpus/tests-sos/test`) are a conformance pass:
about 150 small programs, each exiting nonzero on a wrong answer. Their build is
automake's, so each is built alone with `oshcc` - the C++ ones with `g++` as its
compiler - and run on 4 × 2:

```sh
cp -R corpus/tests-sos/test $W/sos && cd $W/sos
for src in unit/*.c unit/*.cpp spec-example/*.c apps/*.c; do
  p=${src%.*}; cc=; extra=
  case $p in
    unit/mt_lock|spec-example/hybrid_mpi_*) continue ;;   # a helper; MPI programs
    unit/mt_lock_test) extra=unit/mt_lock.c ;;
    spec-example/shmem_ctx) extra=-fopenmp ;;
  esac
  case $src in *.cpp) cc=OSOE_CC=g++ ;; esac
  env $cc $OSHCC -O1 -Iinclude $src $extra -o $p -lm -pthread 2> $p.build \
    || { echo "$p: build failed"; continue; }
  timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./$p > $p.out 2>&1 < /dev/null
  echo "$p: exit $?"
done | tee ~/sos.txt
```

The development VM ran them on two PEs over two logical nodes and on four over
two (`build/scratch/sos/sweep.sh`): 147 built and exited 0 on both on
2026-09-24, and 150 on 2026-09-26 at `e61a172`, after team creation.
**What to expect** of the rest:
- `c11_shmem_test_all_any_some` and `c11_shmem_wait_until_all_any_some` do not
  build: they pass `short *` to the `_all`/`_any`/`_some` generics, which
  OpenSHMEM 1.5 defined over Table 6's AMO types alone - SOS's extension.
- `mt_a2a`, `mt_contention`, `mt_lock_test`, `mt_membar`, `thread_wait`,
  `threading`, `web` and `spec-example/shmem_ctx` exit 1: they need
  `SHMEM_THREAD_MULTIPLE`, and osoe provides `SERIALIZED` (design §4.11).
- `spec-example/shmem_team_context` exits 1: it needs contexts beyond the
  default (§7.3), which step 8 proposes
  (`docs/prompts/phase1-step8-slice1-prompt.md`). The other team tests pass
  since team creation (phase 1 step 7 slice 2, 2026-09-26).
- `shmem_team_b2b_collectives` collects 40 longs into a destination of 10 on
  the VM's 0,0,1,1 map, and osoe stops it by name where the old gather
  corrupted memory silently; whether 4 × 2 trips it is a finding either way.
- `shmemalign` exits 1 with osoe naming the call: it asks `shmem_align` for an
  alignment of 2, which the spec leaves undefined below `sizeof(void *)`.
- `bigget` exits 124 - the loop's 120 s `timeout` - on the cluster: it does
  1,000 gets of 16 MiB a PE, 16 GiB over gigabit Ethernet. Run it alone with
  `./unit/bigget -l 100`; it passes in 22 s at 4 × 2 (2026-09-26).

On eight PEs over four nodes the programs meet shapes the VM did not run them
on; one that fails here and passed there is the finding.

## 9. What to bring back

- the outputs of steps 1, 5, 6, 7 and 8;
- the fixtures step 3 captured, to commit with the tests they feed;
- anything that failed, with its output, and the node it failed on.

## 10. The second session (2026-09-26)

Main `b8d7ad5` (plus that day's documentation), `release`, driven over
`ssh pi-login` from the development Mac, about 45 minutes of cluster time.
Raw outputs: `docs/design/evidence/pi-cluster/2026-09-26/`; the day's record
and the decisions it fed: `docs/decisions/2026-09-26-day.md`. Everything the
first session left ran, and the runbook's expectations held, with two
findings.

**The floor (§5)**, seven pairs on two nodes: the echo's fastest round trip
153-156 µs; the library's get 1.09× of it, put and quiet 1.08×, fetch-add
1.12× (fastest of each pair; the medians 1.09, 1.08, 1.13); the stream 111
against the echo's 112 MiB/s. Tier 3's `pi` link had predicted 1.01-1.06× on
a fast CPU; the Pi's cores cost a further 3-7%, and the 170 µs round trip
every table shows is the Pi's network stack, not the library.

**`osoe-bench` (§6)**, all four lines: 8-byte put and quiet 169-175 µs, get
168-181, fetch-add 162-176; a 1 MiB put 9.1 ms and get 11.2 ms (the get gap of
the first session, unchanged); every stream 111.5 MiB/s from 64 KiB up; a
barrier 178-184 µs at 2, 4 and 16 PEs; sixteen PEs streaming pairwise 440
MiB/s in all, four links at line rate. **Bound against unbound at 4 × 4**:
168.6 against 170.5 µs for the put, every line within 5%. The agent thread
sharing a core with its PE costs nothing measurable here.

**The test suite, split as §2 has it.** Tier 1 on the head node: 95 of 95,
1,485 s of which 1,300 were the three model checks. Tier 2 on one compute node
with `-c4`: 347 of 347 in 44 s, the floor pins and `echo-uring`'s included.
`shmem.get-no-progress.0-1` alone, five repeats: pass, about 7 s each.

**Start-up.** `hello` at 1 × 1 in 0.07-0.10 s, at 4 × 1 in 0.14-0.25 s, at
4 × 4 in 0.23-0.26 s, against 0.18-0.20 s for `srun /bin/true` at 4 × 4: the
library adds about 60 ms at 16 PEs. The first session's guess that a node took
over a second to connect is refuted; that test's 1 s limit was simply tight.

**SOS at 4 × 2**: 149 pass, 2 do not build, 12 exit nonzero, each as §8 names
it - `bigget` new among them, for its loop count at gigabit speed alone.

**The pipelined collectives**, 1 MiB on four nodes, one PE a node
(`osu-*-k*-d*.txt`; a fan-out of 3 is a fan-out of 4 on four nodes):

| | D = 1 | D = 2 | D = 4 | D = 8 |
|---|---|---|---|---|
| broadcast, k = 4 | 36.6 ms | 27.4 | 27.2 | - |
| broadcast, k = 2 | 38.8 | 22.3 | 18.4 | 18.2 |
| reduce, k = 4 | 48.5 | 28.2 | 28.3 | - |
| reduce, k = 2 | 58.4 | 33.5 | 23.0 | 19.4 |

At k = 4 the root sends three copies over one link, 27 ms at 111 MiB/s, and
no D goes below it; at k = 2 it sends two, 18.8 ms, and D = 4 reaches it. So
**over Ethernet the fan-out is a bandwidth decision for large messages**: k =
2 is 1.5× faster than the default at 1 MiB, and 1.45× for the reduction at
D = 8 (design §10, the fan-out item; L48). Small messages pay a hop per level;
the sweep started at 64 KiB, so the crossover is the next session's.

**The allgather.** fcollect of 1 MiB a PE: 28.0 ms at 4 × 1 and 120 ms at
4 × 4, against the first session's tree at 190 and 773 - 6.8× and 6.4×, the
second within 10% of the twelve MiB a node must send at line rate. Of 8 bytes
a PE: 243 µs at 4 × 1 and 269 at 4 × 4 against 190 and 197 - 1.28× and 1.37×,
under the 1.5× at which the two-slot staging would have been built; the author
accepted the cost (`docs/decisions/2026-09-26-day.md`).

**PEs per node.** A barrier 181, 202 and 185 µs at 1, 2 and 4 PEs a node; an
8-byte reduce 184, 193, 193; an 8-byte fcollect 246, 254, 270. Shared memory,
two PEs on one node: a put of 1 byte 0.16 µs, a get 0.04; 1 MiB 103 and 99 µs.

**Finding F1: fcollect at 4 × 2 with 16 KiB blocks takes 63-109 ms a call**
(five runs: 69, 63, 88, 109 ms, and 43.6 under a cyclic layout), against
0.8 ms at 8 KiB and 2.5 ms at 32 KiB; every other shape is smooth, 4 × 1 and
4 × 4 at 16 KiB included, and 4 × 4 at 8 KiB has the same 32 KiB share a
node. On any three of the four nodes the row is normal (1.17 ms without
`pi-002`, 1.11 without `pi-004`). The job makes progress throughout - the
no-progress rule never fires - the NICs count no errors and no drops, and
TCP's `TcpExtTCPTimeouts` on one node climbed 482 → 492 in eight seconds of
the run while the others held at 2-4; every node has fast-retransmitted about
0.03% of its segments since boot. A retransmission timeout is 200 ms or more,
so the average is one every few calls. Not a cable: it needs all four nodes.

**Finding F2** is the fan-out, above.

## 11. Next session

1. **F1**, first: `cd $W/osu/c/openshmem` and

   ```sh
   srun -K1 -N4 --ntasks-per-node=2 ./osu_oshm_fcollect -m 16384:16384 -i 200 > ~/f1.txt &
   srun -N4 --ntasks-per-node=1 --overlap sh -c 'for i in $(seq 40); do echo "$(hostname) $(date +%T) $(nstat -z TcpRetransSegs TcpExtTCPTimeouts | awk "NR>2{printf \"%s=%s \", \$1, \$2}")"; sleep 1; done' > ~/f1-nstat.txt
   ```

   then the same with `tcpdump -i eth0 -w ~/f1-pi-002.pcap port not 22` on
   one node (`srun -N1 -w pi-002 --overlap`, root needed), to see which
   segments are lost and where: the switch dropping an incast at step 1, where
   two nodes' 64 KiB shares arrive at one, or a sender's burst. Try
   `OSOE_AM_CHUNK` and `OSOE_WIRE_BYTES` smaller, and `-N4 --ntasks-per-node=2`
   with `OSOE_COLLECTIVE_PIPELINE=1`.
2. **F2's crossover**: broadcast and reduce from 4 bytes to 64 KiB at k = 2
   and k = 4 (`-m :65536`), to set the size at which a fan-out of 2 should
   take over.
3. **The vanished node** (§7), with its PoE port power-cycled after.
4. After step 8's first slice lands: SOS's `shmem_team_context` must pass.

Bring back `f1*.txt`, the pcap, the two sweeps, and anything that failed.
