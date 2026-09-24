# osoe

OpenSHMEM Over Ethernet: an implementation of OpenSHMEM 1.6 for commodity Ethernet, built on io_uring and
designed to build entirely from a Linux distribution's own packages — no fabric
libraries.

**Status:** phase 1, step 4 of 4 (design §7.1). Jobs run on one node or across
nodes over TCP and io_uring, started by `srun` or by `osoe-launch`:
initialization and thread levels up to `SHMEM_THREAD_SERIALIZED`, the symmetric
heap allocator in full, contiguous RMA (`shmem_putmem`, `shmem_getmem`,
`shmem_putmem_signal`), `shmem_ptr`, every atomic type of specification Tables
6–8 with the C11 type-generic interfaces, the point-to-point waits and tests
including the multiple-element forms, signalling operations, memory ordering,
`shmem_barrier_all`, `shmem_sync_all` and `shmem_team_sync`,
`shmem_global_exit`, and the mandatory surface of §7.4 including the `pshmem_`
profiling interface. A PE or a node that dies ends the whole job (§3.15).

Not yet: symmetric statics, which are refused with a diagnostic (§4.3);
collectives other than the barriers - broadcast, reductions, collect and
alltoall; typed and strided RMA and the nonblocking forms; locks; contexts
other than the default; and `SHMEM_THREAD_MULTIPLE` (§7.5, §4.11).

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
| `OSOE_HEAP_BASE` | no | Address for each PE's own heap, e.g. `0x100000000000`, if the defaults are taken |
| `OSOE_HEAP_SHIFT` | no | A test mode: a size, a whole number of pages, e.g. `2m`. Each PE maps its own heap that far times its rank past the base, so no two PEs share a symmetric address (design §8.2) |
| `OSOE_WATCHDOG` | no | `1` or `0`: the development watchdog, which aborts loudly when a PE dies while others wait, instead of hanging. On in the `debug`, `asan` and `tsan` builds, off in `release` (design §3.15) |
| `OSOE_LIVENESS` | no | `1` or `0`: whether each PE's agent watches for a local PE's death, with pidfds, in every build (design §3.15). On unless set to `0`, which only the tests of the watchdog need |
| `OSOE_KEEPALIVE` | no | `IDLE,INTERVAL,COUNT` or `0`: TCP keepalive on the connections between nodes, in seconds, seconds and probes; default `60,10,6` (design §3.15). Jobs of more than one node only |

Rules:
- The job id and step id name the job's shared memory
  (`/dev/shm/osoe.<job>.<step>.…`). Two jobs running at once with the same pair
  collide, and the second fails at init.
- `OSOE_GTIDS[OSOE_LOCALID]` must equal `OSOE_PROCID`.
- The names mirror Slurm's `SLURM_PROCID`, `SLURM_GTIDS`, and so on (design §6.1, §6.2).

For example, two PEs from a shell:

```sh
export OSOE_STEP_NUM_TASKS=2 OSOE_NODEID=0 OSOE_GTIDS=0,1 OSOE_JOB_ID=$$ OSOE_STEP_ID=0
OSOE_PROCID=0 OSOE_LOCALID=0 ./my_program &
OSOE_PROCID=1 OSOE_LOCALID=1 ./my_program
wait
```

A job that crashes can leave `/dev/shm/osoe.<job>.*` behind. Remove it before
reusing the same job id.

## Documentation

- [Design notes](docs/design/shmem-ethernet-design.md)
- [Specification reference](docs/reference/README.md)

## License

Not yet chosen. Until a license is added, no rights are granted to use, copy,
modify, or distribute this code.
