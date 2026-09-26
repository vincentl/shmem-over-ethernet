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
- The governor matters to every time below: `ondemand` clocks a core down between trips, so write down which one ran.
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
gather; its generic internode phase is the one whose latency grows faster
than log N (design §5.5), so its line at 16 PEs is the one to keep.

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
autoconf, which the cluster need not have; the stamps go back in order first:

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
two (2026-09-24, `build/scratch/sos/sweep.sh`): 147 built and exited 0 on both.
**What to expect** of the rest:
- `c11_shmem_test_all_any_some` and `c11_shmem_wait_until_all_any_some` do not
  build: they pass `short *` to the `_all`/`_any`/`_some` generics, which
  OpenSHMEM 1.5 defined over Table 6's AMO types alone - SOS's extension.
- `mt_a2a`, `mt_contention`, `mt_lock_test`, `mt_membar`, `thread_wait`,
  `threading`, `web` and `spec-example/shmem_ctx` exit 1: they need
  `SHMEM_THREAD_MULTIPLE`, and osoe provides `SERIALIZED` (design §4.11).
- `shmem_team_reuse_teams`, `unit/shmem_team_translate` and the examples
  `shmem_team_split_strided`, `shmem_team_context` and, on four PEs or more,
  `shmem_team_sync` exit 1: they create teams, which osoe does not yet (§5.7;
  `docs/prompts/phase1-step7-slice1-prompt.md` proposes it).
- `shmemalign` exits 1 with osoe naming the call: it asks `shmem_align` for an
  alignment of 2, which the spec leaves undefined below `sizeof(void *)`.

On eight PEs over four nodes the programs meet shapes the VM did not run them
on; one that fails here and passed there is the finding.

## 9. What to bring back

- the outputs of steps 1, 5, 6, 7 and 8;
- the fixtures step 3 captured, to commit with the tests they feed;
- anything that failed, with its output, and the node it failed on.

## 10. Next session

The first session (2026-09-24) ran §1 to §4, §7 and most of §8; its results
are in the status page and in `docs/decisions/2026-09-24-day.md`. What it left,
in order, with `W=~/osoe-corpus` and `OSHCC` as in §8:

1. **The test suite, split as §2 now has it**: tier 1 on the head node, tier 2
   on a compute node whose image has neither a compiler nor `liburing-dev`.
2. **`shmem.get-no-progress.0-1` alone.** Its one-second limit fired while a
   second logical node was still connecting; its limit is now five seconds.
   Alone, it tells contention from a slow start:

   ```sh
   srun -N1 -n1 -c4 ctest --test-dir build/release -R get-no-progress --repeat until-fail:5 --output-on-failure
   ```

3. **How long a job takes to start**, one node and four, with §8's `hello`:

   ```sh
   time srun -K1 -N1 --ntasks-per-node=1 $W/examples/hello > /dev/null
   time srun -K1 -N4 --ntasks-per-node=4 $W/examples/hello > /dev/null
   ```

4. **§5, the floor**, and **§6, `osoe-bench`**, all five lines: the first
   session brought back neither, and they say how close a round trip is to
   the link's own.
5. **§8's SOS test suite** at 4 × 2: 147 programs pass and 16 fail, each for
   the reason §8 names. It ran verbatim at this shape in the VM.
6. **PEs per node.** A pair on one node, which is the shared-memory path, and
   three collectives at one, two and four PEs a node:

   ```sh
   cd $W/osu/c/openshmem
   srun -K1 -N1 --ntasks-per-node=2 ./osu_oshm_put heap > ~/osu-put-1x2.txt
   srun -K1 -N1 --ntasks-per-node=2 ./osu_oshm_get heap > ~/osu-get-1x2.txt
   for ppn in 1 2 4; do
     for p in barrier reduce fcollect; do
       srun -K1 -N4 --ntasks-per-node=$ppn ./osu_oshm_$p -m :65536 > ~/osu-$p-4x$ppn.txt
     done
   done
   ```

7. Optionally, **§7's vanished node**, with its PoE port power-cycled after.

Bring back what §9 lists, and `sos.txt`, the `osu-*-1x2.txt` and `osu-*-4x*.txt`
files, both halves of the test suite's summary, and the two start-up times.
