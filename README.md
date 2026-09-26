# osoe

OpenSHMEM Over Ethernet: an implementation of OpenSHMEM 1.6 for commodity Ethernet, built on io_uring and
designed to build entirely from a Linux distribution's own packages — no fabric
libraries.

**Status:** phase 1, through step 7 (team creation), step 8's first slice
(contexts), step 9's first slice (block-strided RMA) and step 10's (the
prefix scans), and step 8's second and third, `SHMEM_THREAD_MULTIPLE` with
lock-free issue on shared contexts (design §7.1, §4.11). Jobs run on one node or across
nodes over TCP and io_uring, started by `srun` or by `osoe-launch`:
initialization and every thread level, `SHMEM_THREAD_MULTIPLE` when
`shmem_init_thread` asks for it (`shmem_init` gives `SHMEM_THREAD_SERIALIZED`), the symmetric
heap allocator in full, RMA - `shmem_putmem` and `shmem_getmem`, and every type
and size of specification Table 5 with their single-element, strided,
block-strided and nonblocking forms, and put-with-signal in each - `shmem_ptr`,
every atomic type
of specification Tables 6–8, the C11 type-generic interfaces of both, the
deprecated atomic names of Annex F.2.12, the point-to-point waits and tests
including the multiple-element forms,
signalling operations, memory ordering, `shmem_barrier_all`, `shmem_sync_all`
and `shmem_team_sync`, team creation - `shmem_team_split_strided`,
`shmem_team_split_2d` and `shmem_team_destroy`, with every team routine and
collective on the teams they make - the team-based reductions of every operator and type of
specification Table 10, broadcast, collect, fcollect, alltoall and alltoalls,
with their C11 interfaces, the deprecated
active-set forms - the `_to_all` reductions of Table 11, `shmem_broadcast32` and
`64`, the collects, fcollects and alltoalls of 32 and 64 bits, `shmem_barrier`
and `shmem_sync` - over any active set, the first-come, first-served distributed locks, `shmem_global_exit`,
the mandatory surface of §7.4 including the `pshmem_` profiling interface, and
symmetric statics - a program's global and static variables as symmetric
objects - in a program built with the `oshcc` wrapper (§4.3). A PE or a node
that dies ends the whole job (§3.15).

Contexts - `shmem_ctx_create`, `shmem_team_create_ctx` and every RMA, AMO and
ordering routine on them, with team-relative PE numbering - are there; a
context created `SHMEM_CTX_PRIVATE` or `SHMEM_CTX_SERIALIZED` is serial, one
call at a time. Under `SHMEM_THREAD_MULTIPLE` any other context, the default
included, takes any number of threads at once: they issue without a lock,
and their quiets on it take turns.

The prefix scans, `shmem_sum_inscan` and `shmem_sum_exscan`, over every type
SUM takes, are there too (design §5.8).

Nonblocking gets across nodes overlap, many in flight before the quiet that
completes them; a nonblocking put returns once its source may be reused, as a
blocking one does.

## Requirements

- Linux 6.7 or newer
- liburing 2.6 or newer
- CMake 3.25 or newer, pkg-config, and a C11 compiler

Debian 13 (Trixie) and Raspberry Pi OS Trixie provide all of these from their own
repositories. Older releases are not supported, even where the kernel is new
enough: see the design notes, §1.

## Building

Builds use CMake presets and run on Linux only. Each preset builds under
`build/<preset>`, which git ignores:

```sh
cmake --workflow --preset debug      # configure, build, and test
```

Presets: `debug`, `release` (optimized, with debug info), `asan`
(AddressSanitizer and UndefinedBehaviorSanitizer), and `tsan` (ThreadSanitizer).

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

## Building a program

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

## Running a program

### With `srun`

```sh
srun -N4 --ntasks-per-node=4 ./my_program
```

No PMI plugin is involved: each node's lead finds the others over TCP, on a
port it derives from the job and step ids (`OSOE_PORT_RANGE`, default
17000–26999), and then connects to each on a port the kernel chooses, so TCP
must be open between the nodes, not only that range. Every node must resolve
the names in the step's node list, and the program must be at the same path on
every node. Use `srun -K1` (or `KillOnBadExit=1`), so that a task that fails
at start-up ends the rest rather than leaving them waiting (design §3.15), and
`OSOE_NO_PROGRESS_TIMEOUT=60` while a cluster is new, so that a wait that stops
moving says what it waits for. `OSOE_JOB_SECRET`, 32 hexadecimal digits
exported before `srun`, keeps other hosts out of the job's rendezvous (design
§6.2). A program run straight from a batch script, not through `srun`, is
refused, since it is no task of a step. `tools/launch/capture-slurm-fixtures.sh`
records a step's environment for the bootstrap's tests (design §6.1, §8.2).

### With `osoe-launch` (development and tests)

`build/<preset>/tools/launch/osoe-launch` starts the processes of a job on this
machine and sets every bootstrap variable for them:

```sh
build/debug/tools/launch/osoe-launch -n 4 -- ./my_program
build/debug/tools/launch/osoe-launch -n 6 -m 0,0,1,1,2,2 -- ./my_program
```

`-m` is a logical node map (design §8.1): one node id per rank, each node's
lead listening on its own loopback address, so one machine runs a job of
several nodes. `-s` names the variables as `srun` does, so the job takes the
bootstrap's Slurm path.

It fails if any process fails, if the timeout (`-t SECONDS`, default 60)
expires, or if the job leaves shared memory behind. `-e RANK:NAME=VALUE` sets a
variable for one rank only. `-k` leaves the surviving processes alone when one
fails, instead of terminating them, so that what ends the job is the library's
own reaction rather than the launcher's.

### Without a launcher

Start one process per PE yourself, with these variables set in each. A program
started with none of them fails at `shmem_init` with "no launcher environment".
There is no single-PE default.

| Variable | Required | Value for a one-node job of N PEs |
|---|---|---|
| `OSOE_PROCID` | yes | This PE's rank, `0` … `N-1` |
| `OSOE_STEP_NUM_TASKS` | yes | `N` |
| `OSOE_NODEID` | yes | `0` |
| `OSOE_LOCALID` | yes | This PE's position in `OSOE_GTIDS`; `0` is the local lead |
| `OSOE_GTIDS` | yes | The ranks on this node, comma-separated: `0,1,…,N-1` |
| `OSOE_JOB_ID` | yes | A number unique among jobs running on this machine at once |
| `OSOE_STEP_ID` | yes | A number; `0` is fine |
| `SHMEM_SYMMETRIC_SIZE` | no | Heap size per PE, e.g. `64m` or `2g`; default 256 MiB. Must agree across PEs |
| `OSOE_NO_PROGRESS_TIMEOUT` | no | Seconds a wait may go without progress before it ends the job naming what it waited for; unset, no wait gives up (design §3.15). Replaces `OSOE_BOOTSTRAP_TIMEOUT`, which is retired |
| `OSOE_HEAP_BASE` | no | Address for each PE's own heap, e.g. `0x100000000000`, if the defaults are taken. A multiple of the page size, which is 16 KiB on a Raspberry Pi 5 |
| `OSOE_HEAP_SHIFT` | no | A test mode: a size, a whole number of pages, e.g. `2m`. Each PE maps its own heap that far times its rank past the base, so no two PEs share a symmetric address (design §8.2) |
| `OSOE_MAX_TEAMS` | no | Teams in existence job-wide at once, the three predefined included; `4` to `1024`, default `64`. Must agree across PEs: it sizes every per-team structure, and a `shmem_team_split_2d` claims a number for every row and column at once (design §5.7) |
| `OSOE_MAX_CONTEXTS` | no | Contexts a PE may have at once, `SHMEM_CTX_DEFAULT` included; `1` to `4096`, default `64`. Past it `shmem_ctx_create` returns nonzero. Must agree among the PEs of a node (design §4.11) |
| `OSOE_MAX_THREADS` | no | Application threads of a PE in the library at once, the main thread included; `1` to `1023`, default `16`. One more thread ends the job, naming this variable. Must agree among the PEs of a node (design §3.14, §4.11) |
| `OSOE_GET_LANDING_BYTES` | no | Bytes of each PE's landing zone, in `/dev/shm`, which its gets' replies share in extents of 1 KiB; a multiple of `1024` from `1024` to `67107840`, default `262144` (256 KiB). Past it a get completes what is in flight before it asks for more (design §4.6, §4.11) |
| `OSOE_KEEPALIVE` | no | `IDLE,INTERVAL,COUNT` or `0`: TCP keepalive on the connections between nodes, in seconds, seconds and probes; default `60,10,6` (design §3.15). Jobs of more than one node only |

Rules:
- The job id and step id name the job's shared memory
  (`/dev/shm/osoe.<job>.<step>.…`). Two jobs running at once with the same pair
  collide, and the second fails at init.
- `OSOE_GTIDS[OSOE_LOCALID]` must equal `OSOE_PROCID`.
- The names mirror Slurm's `SLURM_PROCID`, `SLURM_GTIDS`, and so on (design §6.1, §6.2).
- `OSOE_WATCHDOG` and `OSOE_LIVENESS` are retired and ignored, with a notice: every
  build watches for a local PE's death with pidfds, and a PE the job's end finds
  waiting names the routine it waited in (design §3.15).

For example, two PEs from a shell:

```sh
export OSOE_STEP_NUM_TASKS=2 OSOE_NODEID=0 OSOE_GTIDS=0,1 OSOE_JOB_ID=$$ OSOE_STEP_ID=0
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

## Documentation

- [Design notes](docs/design/shmem-ethernet-design.md)
- [Specification reference](docs/reference/README.md)

## License

osoe is free software under the GNU Lesser General Public License, version 3
([COPYING.LESSER](COPYING.LESSER)), which supplements the GNU General Public
License, version 3 ([COPYING](COPYING)). A program linked against the
library may carry any license its authors choose; changes to the library
itself are shared under the same terms.
