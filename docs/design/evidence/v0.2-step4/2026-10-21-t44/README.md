# T44's evidence (2026-10-21, run again 2026-10-24): Open MPI and osoe in one program

The item (`docs/prompts/v0.2-open-items.md`, T44): "MPI in the same program,
untried" - the README said so under its limits - with the author's leave of
2026-10-20 to install Open MPI by `apt` in the `osoe` development VM. A trial:
no library code, test, preset or CMake file is changed, and nothing in the
build or the suite needs MPI.

## What it found

**It works.** One program that starts MPI and osoe (`mpi-and-shmem.c`) ran 37 of
its 39 launch cases clean (the other two are refusals it was meant to meet), and
60 repeated starts, with Open MPI 5.0.7 as Debian 13 packages it:

- `MPI_Init` and `shmem_init` in either order, and ended in either order (the
  four pairings), and `shmem_finalize` and `shmem_init` again, three times,
  with MPI running between them;
- `MPI_Allreduce` and `MPI_Sendrecv` on private memory, on the symmetric heap
  and on the program's statics, a megabyte at a time; a window of MPI's
  one-sided calls over a
  `shmem_malloc` buffer, with `MPI_Put` and `MPI_Get` beside `shmem_int_p` into
  the same memory; `shmem` reductions, puts, atomics and barriers alternated
  with MPI's barriers and collectives;
- `MPI_THREAD_MULTIPLE` beside `SHMEM_THREAD_MULTIPLE`, two progress helpers
  across two logical nodes (`c6`; on one node none starts, `b14`), Open MPI's
  TCP transport instead of its shared memory, and with TCP's progress
  thread;
- 1 to 16 PEs on one node (8 and 16 on four CPUs), and 4 PEs in two logical
  nodes and in four, and 16 in four - the two jobs of 16 PEs at 5 rounds, the
  rest at 20.

**Three ways to start it.**

| Way | Command | How osoe is bootstrapped | MPI between the PEs |
|---|---|---|---|
| Slurm | `srun -K1 --cpu-bind=none --mpi=pmix_v5 -n N ./prog` | `SLURM_` variables, as without MPI | yes |
| `mpirun` | `mpirun --bind-to none -n N open-mpi-bootstrap.sh ./prog`; `T44_NODES=K` before it makes K logical nodes | the wrapper makes the `OSOE_` variables of the README's "Starting PEs without a launcher" from Open MPI's | yes |
| `osoe-launch` | `osoe-launch -n N -- ./prog` | `osoe-launch`'s | no: every PE is an MPI singleton, a world of one |

Under `srun` with PMIx osoe and Open MPI read what Slurm gives each (`d1` to `d3`,
`d5`); with `--mpi=none` every rank is an MPI singleton, as under `osoe-launch`
(`d4`). Under `mpirun` nothing sets osoe's variables, and a program started plain
is refused at `shmem_init` (`b2`): `osoe: no launcher environment: neither
OSOE_PROCID nor SLURM_PROCID is set. Start programs with srun or osoe-launch, or
set the OSOE_ bootstrap variables of design §6.2`. The wrapper maps
`OMPI_COMM_WORLD_RANK` and `_SIZE` to `OSOE_PROCID` and `OSOE_STEP_NUM_TASKS`,
cuts the ranks into K nodes of N / K (`OSOE_NODEID`, `OSOE_STEP_NUM_NODES`,
`OSOE_LOCALID`, `OSOE_GTIDS`, and `OSOE_STEP_NODELIST`, `127.0.0.[1-K]` for K
logical nodes and the host's name for one), and takes mpirun's pid as
`OSOE_JOB_ID` and its start time as `OSOE_LAUNCH`. On one real node it checks
`OMPI_COMM_WORLD_LOCAL_RANK` and `_LOCAL_SIZE` against its own arithmetic.
`mpirun`'s variables carry no node numbers and no host list
(`mpirun-variables.txt`, from `s4`), so several machines would need the wrapper to
be given both; not tried. `osoe-stat JOB` takes mpirun's pid and reads the job
(`o1`).

**What it costs**, each with the files that show it (`runs/`):

1. **The deadlock detector is blind in a process that links this Open MPI** (`w1`
   to `w9`, `u1` to `u7`). It proves a deadlock only when every thread of a PE but
   the agent and the helpers sleeps in a library wait (`shmem/src/deadlock.c`,
   `threads_asleep`); a thread of the program's own that sleeps elsewhere is one
   the library cannot see, so nothing is judged deadlocked, as the README says of
   a worker or an idle OpenMP pool. Two such threads come with MPI. Debian's
   `libmpi` needs UCX's `libucs`, which starts a thread named `fuse` as it is
   loaded - UCX's virtual file system, serving nothing here - in every process
   that links the program, started MPI or not (`f6`, `f7`; `t1` against `t0`); and
   `MPI_Init` starts one more, which `MPI_Finalize` ends (`u2`, `u3`). So a
   deadlock in osoe alone is found in 4 s (`OSOE_DEADLOCK_SECONDS=3`) by the
   program built without Open MPI (`w0`), and never by the same program linked
   with it, before `MPI_Init`, between the two calls or after (`w1` to `w5`, `w8`,
   `w9`: the job waits until something ends it - `osoe-launch`'s limit in `w1` and
   `w5`, the trial's own `timeout` under `mpirun`, which has none).
   `UCX_VFS_ENABLE=n` ends the first thread: then it is found again before
   `MPI_Init` and after `MPI_Finalize` (`u4`, `u6`, `u7`, 4 to 5 s) and not
   between (`u5`). A PE blocked in MPI cannot confuse it into a wrong proof (`w7`:
   three PEs in `MPI_Barrier` or computing, one in `shmem_int_wait_until`, twelve
   seconds, `OSOE_DEADLOCK_SECONDS=2`, no report) - it never proves anything while
   MPI runs - and a deadlock across the two (PE 0 in `MPI_Barrier`, the others in
   `shmem_barrier_all`, `w6`) is found by neither library.
2. **`shmem_global_exit(status)` under `mpirun` ends the job with 1**, not
   `status` (`e-global-exit-*-mpirun`, `g7`): the PEs it ends leave without
   `MPI_Finalize`, and `prterun` names one of them - rank 0 in two runs, rank 2 in
   the third, never the caller, PE 1 - as "exiting improperly". Under `srun` and
   `osoe-launch` the status is 3 as asked (`g8`, `e-global-exit-*-srun`,
   `-singletons`); `MPI_Abort(3)` reaches all three as 3.
3. **A singleton leaves a file in `/dev/shm`**: `sm_segment.<host>.<uid>.<id>.0`,
   one a process, after a clean `MPI_Finalize` too (`a2` to `a7`, `d4`, `g1`,
   `g10`): 16,777,216 bytes apparent, 72 KiB allocated (`stat`, measured beside
   the second run; `docs/decisions/2026-10-24.md`). A job under `mpirun`, or under
   `srun` with PMIx, leaves none, however it ends (`g2` to `g9`, and the `e-`
   cases under both). osoe's own names are gone in every case, as its README says.

## Smaller findings

- **Signals.** osoe installs no handler (`t0`: none). Linking `libmpi` installs
  five, from `libucs` as it loads: `SIGHUP`, `SIGILL`, `SIGBUS`, `SIGFPE`,
  `SIGSEGV`; `MPI_Init` adds `SIGABRT`, once, from `libopen-pal`, and it stays
  after `MPI_Finalize` (`t1` to `t6`). They print a backtrace and the process
  dies by the signal: a `SIGSEGV` raised by PE 1, two seconds in, ended the job
  at once with status 139 under all three launchers - osoe's node saying `local
  PE 1 (rank 1, pid N) has ended without finalizing; ending the job (§3.15)`
  under `mpirun` and `srun`, `osoe-launch` naming the signal under itself, and
  `prterun` that rank 1 "exited on signal 11" (`e-crash-*`).
- **Fork.** `MPI_Init` forks nothing and execs nothing in either mode - under
  `strace -ff` of a rank, the only processes are the wrapper's own `awk` and
  `hostname`; three threads are made, UCX's, Open MPI's and osoe's agent
  (`f4`, `f5`). A program that forks after both libraries started - `system()`,
  a child that leaves by `exit(0)`, one by `_exit(0)` - ends clean under
  `mpirun` and `osoe-launch`, the two it ran under: osoe's exit handler and
  `shmem_finalize` check the pid (`shmem/src/init.c`), and Open MPI's make no
  trouble. The child's write to a static is
  the parent's too, osoe's statics being shared after a fork, as `oshcc`'s
  comment warns (`f1` to `f3`).
- **Progress threads.** Open MPI adds one thread a PE (two for `fuse`), osoe's
  agent one; `MPI_THREAD_MULTIPLE` changes nothing (`t7`), and helpers hold the
  wires to other nodes, so on `t8`'s one node none starts.
  `btl_tcp_progress_thread=1` is in `k1` (below).
- **Ports.** Each rank's TCP transport listens from port 1024 up, on every
  address, one IPv4 and one IPv6 port a rank, while the job holds (one listing,
  five seconds in), even when shared memory carries the traffic; `prterun` listens
  on three ephemeral ports. osoe's rendezvous listeners (17000-26999) and its
  mesh listeners (the kernel's ports) are closed once the start is over; the
  mesh connections stay (`n1`). Nothing overlaps by default. Made to overlap, on
  purpose (`OSOE_PORT_RANGE=17000-17003` and Open MPI's range the same four),
  each fails by name, whichever started first: osoe `found none of its 32
  candidate ports free in OSOE_PORT_RANGE=17000-17003; choose a range this host
  does not use`, or Open MPI `bind() failed: no port available in the range
  [17000..17004]` and `MPI_INIT has failed because at least one MPI process is
  unreachable` (`n2`).
- **The statics.** `oshcc` links `-no-pie`, so the program's references to
  `MPI_COMM_WORLD`, `MPI_INT` and the rest become copy relocations of Open MPI's
  variables. GNU ld places them after `__osoe_statics_bss_end`, so
  osoe's remapping of the statics does not touch them (`link-facts.txt`). With
  lld the script's own comment says they would be gathered; not tried.
- **Names.** The package puts `oshcc`, `oshrun`, `oshCC`, `oshcxx`, `oshc++`,
  `oshfort` and `oshmem_info` in `/usr/bin`, and an OpenSHMEM `shmem.h` beside
  `mpi.h` in the include directory `mpicc` adds. `OSOE_CC=mpicc oshcc` puts osoe's
  `-I` first and Open MPI's after (`build-commands.txt`), so `shmem.h` is osoe's;
  but a PATH that finds `/usr/bin/oshcc` first, or `mpicc -I` ahead of osoe's,
  builds against the other library's header.
- **Launchers.** `mpirun`'s own binding and `--bind-to none` ran alike (`p`);
  `SIGTERM` or `SIGINT` to `mpirun`, six seconds in, ended the job at once,
  status 1, nothing left running but a zombie reaped a moment later
  (`e-sigterm-to-mpirun`, `e-sigint-to-mpirun`);
  `SIGTERM` to `osoe-launch` 143 (`e-sigterm-to-osoe-launch`). A rank count that
  does not divide into the logical nodes is refused by the wrapper by name (`c7`).

## What ran

The development VM `osoe` (aarch64, 4 CPUs, 7.7 GiB, Debian 13), under
`/tmp/osoe-vm.lock` for every build and run. A Sonnet agent wrote the trial and
ran it on 2026-10-21, Linux 6.12.95, `main` at `efcf1fa5`; it changed the scripts
after its last runs - four cases renamed, `build.sh`'s check of the include order
and `probes.sh`'s section `k` added - and was stopped by its session's limit
before it committed; its outputs are in the history (`f5208915`). `runs/` holds
the second run, whole, by the lead on 2026-10-24 from 12:30 to 12:38 UTC and
section `s` at 12:57, Linux 6.12.113 after the VM's kernel update, of the tree
that merged - `main` at `f506b95c` with this directory, packed by `git archive`
and built with the release preset, 0 warnings (the library's version string still
reads 0.1.0). Every case ended as in the first run, with the same exit status and
the same PEs. Open MPI 5.0.7 from `apt install openmpi-bin libopenmpi-dev` (left
installed), with `libpmix2t64` 5.0.7, `libucx0` 1.18.1, `libevent` 2.1.13 and
`libhwloc15` 2.12.0; Debian's `/etc/openmpi/openmpi-mca-params.conf` makes the
point-to-point layer `ob1` over `self`, `sm` and `tcp`, not UCX. Slurm 24.11.5,
one node of four CPUs, its `pmix_v5` plugin (`runs/facts.txt`). The jobs ran as
the VM's one user with `SHMEM_SYMMETRIC_SIZE=64m`.

| File | What it is |
|---|---|
| `mpi-and-shmem.c` | The program: starts and ends the libraries in the order it is told, then runs a mode - `check`, `sessions`, `window`, `hold`, `fork`, `crash`, `mpi-abort`, `global-exit`, `plain-exit`, and for the detector `benign-wait`, `deadlock-osoe`, `deadlock-mixed`. Its header lists the settings. Built with `-DT44_WITHOUT_MPI` and `without-mpi.h` it is the baseline with no Open MPI linked. |
| `open-mpi-bootstrap.sh` | The wrapper for `mpirun`. |
| `build.sh`, `facts.sh` | Builds the program and the baseline with `OSOE_CC=mpicc oshcc` and plain `oshcc`, and records the link; records the machine, versions and Open MPI's settings. |
| `launches.sh`, `probes.sh`, `lib.sh` | The cases: `launches.sh` sections `a` (osoe-launch), `b` (mpirun), `c` (logical nodes), `d` (srun), `s` (a first look, and the variables `mpirun` gives a rank); `probes.sh` sections `t` threads and signals, `n` ports, `w` the detector, `f` fork, `e` jobs that end badly, `m` windows, `g` leftover names, `u` UCX's file system off, `p` binding, `o` osoe-stat, `k` threads again, `r` repeated starts. `lib.sh` runs a case under `timeout`, says how it ended and removes what it left. |
| `runs/` | The outputs of the second run, of the scripts as they are here: `launches.txt` and `probes.txt` and `repeats.txt` (one line a case: exit status, time, PEs that said their check passed, names left in `/dev/shm`), each case's own output, the threads and signals (`threads-and-signals.txt`, `threads-without-the-vfs.txt`), the sockets (`n1-*-sockets.txt`), `strace`'s tree and summary (`strace-*`), the thread stacks (`f6`, `f7`, `k2`), `link-facts.txt`, `build-wrong-order.txt` (the `shmem.h` the compiler finds with Open MPI's `-I` first), `mpirun-variables.txt` (`s4`'s `OMPI_`, `PMIX_`, `PRTE_`, `OPAL_`, `SLURM_`, `OSOE_` and `SHMEM_` lines; its whole environment is not kept). |

The scripts take the osoe tree and the output directory as arguments and write no
path of the repository's: in `runs/`, `<repo>` is the tree the VM built (the
archive) and `<out>` the output directory. NUL bytes in four outputs of Open MPI
(`e-mpi-abort-*`, `g6`, `n2-same-ports-shmem-mpi`) are removed. The 60 outputs of
`repeats.txt` are kept for the first start of each of the four cases; the other 56
say what that line says. In `probes.txt`, "still running" after
`n2-same-ports-mpi-shmem`, `w6` and `e-sigterm-to-mpirun` is a rank in the middle
of its exit, its command line already gone (`[mpi-and-shmem]`), or a zombie
(`<defunct>`), which `lib.sh` then kills by pid; nothing of the run's was left in
the VM after it. A case's "PEs ok" counts the lines `check: N rounds, ok`, which
only `check` and `sessions` print.

## Not done, and not known

- One Open MPI (Debian's, with UCX and libfabric), one machine, aarch64. Another
  build may start no thread at load; x86-64, MPICH and several machines are
  untried. Open MPI 4's `orted` and its singleton start are not this one's.
- Speed was not measured: at 8 and 16 PEs on four CPUs everything finished in
  under a second, which is all the trial can say.
- `oshcc` with lld, a position-independent program (`-fpie -pie`, which the
  wrapper allows), and `MPI_Comm_spawn` are untried.
- That UCX's thread is the only one the detector cannot see before `MPI_Init`
  is known from the threads listed (`t0`, `u1`) and not from UCX's code.
