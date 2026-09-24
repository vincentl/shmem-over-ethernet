# osoe

OpenSHMEM Over Ethernet: an implementation of OpenSHMEM 1.6 for commodity Ethernet, built on io_uring and
designed to build entirely from a Linux distribution's own packages — no fabric
libraries.

**Status:** phase 1 in progress. **A single node is complete** (design §7.1 step
1): initialization and thread levels, teams and contexts, the symmetric heap
allocator in full, RMA, `shmem_ptr`, every atomic type of specification Tables
6–8 with the C11 type-generic interfaces, the point-to-point waits and tests
including the multiple-element forms, signalling operations, memory ordering,
`shmem_barrier_all` and `shmem_sync_all`, and the mandatory surface of §7.4
including the `pshmem_` profiling interface. 526 routines; 82 tests in tiers 1
and 2.

Not yet: anything between nodes. The network backend is §7.1 step 2 onward.
Symmetric statics are not supported and are refused with a diagnostic (§4.3).

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

**Status:** one node only. A job whose environment describes more than one node
fails at `shmem_init` with a message saying so, as does a Slurm launch.

### With `osoe-launch` (development and tests)

`build/<preset>/tools/launch/osoe-launch` starts L processes of a one-node job and
sets every bootstrap variable for them:

```sh
build/debug/tools/launch/osoe-launch -n 4 -- ./my_program
```

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
