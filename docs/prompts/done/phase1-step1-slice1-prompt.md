# Implement phase 1, step 1, first slice: one node, no network

You are starting the first real implementation work on **osoe** (OpenSHMEM Over
Ethernet). The design is settled and recorded; your job is to turn a thin, fully
working slice of it into code, with a small design step first.

## Context

- **Repository:** `/Users/vincent/Desktop/Maker Space/shmem-over-ethernet`.
- **Design record:** `docs/design/shmem-ethernet-design.md` (~2,500 lines). Each
  section states a decision, its rationale, and rejected alternatives. §9 lists
  load-bearing invariants; §10 lists closed decisions and open questions. It has
  just been revised after an adversarial review (`docs/reviews/archive/design-soundness.md`).
- **Specification:** `docs/reference/OpenSHMEM-1.6.pdf` (local, gitignored;
  `pdftotext -layout` works on the host). Cite it as §9.x.y.
- **What exists:** a CMake build with `debug`, `release`, `asan`, and `tsan`
  presets; `libosoe` with `shmem.h`, the spec version constants, and
  `shmem_info_get_version`/`shmem_info_get_name`; tests `shmem.info` and
  `shmem.exports` (fails if the library exports anything outside the API);
  `tools/probe/`, a kernel-facility probe registered as test `env.probe`.
- **Linux environment:** the host is macOS. A Lima VM named **`shmem`** (Debian 13,
  kernel 6.12, aarch64, 4 CPUs) mounts the repository at the same absolute path.
  From the repository root, run everything Linux-side with
  `limactl shell shmem <command>`, for example
  `limactl shell shmem cmake --workflow --preset debug`. The presets refuse to run
  on macOS.

**Read these design sections before planning:** §1 (dependency budget), §3.12
(agent threads), §3.14 (control segment layout rules), §3.16 (memory-order
discipline), §4.1, §4.2, §4.4 (per-PE heaps, addressing, allocator), §4.6 (memory
stores and the serializing store), §4.8 (`shmem_ptr`), §5.3–§5.5 (intranode
barrier counters, including the single-node release path), §6.1, §6.2, §6.4
(bootstrap, node-local rendezvous, lifecycle), §7.1 (sequencing), §8.1–§8.2
(testing), §9, and §10.

## Rules

These are project decisions, not preferences.

- **Atomics (§3.16):** ISO C11 `<stdatomic.h>` only. Every access to a shared
  protocol word is an `atomic_*_explicit` call naming one of the four ISO orders:
  loads `acquire`; CAS and `fetch_add`/`fetch_sub` `seq_cst`; single-writer
  publication stores `release`; **both** stores of a two-sided protocol `seq_cst`.
  No implicit-order calls, no `memory_order_relaxed` on protocol words, no GCC
  extension orders (`acq_rel`, `consume`), no compiler intrinsics, no `__builtin_*`,
  no `__atomic_*`/`__sync_*`, no assembly. Use C23 `<stdbit.h>` for bit operations
  (it builds under `-std=gnu11` with glibc 2.41). Futex words are `_Atomic uint32_t`
  with a `_Static_assert` on size and alignment.
- **Dependencies (§1):** libc and the distribution's liburing only; nothing new at
  build time beyond what the presets already use.
- **Futex deadlines are absolute** (§3.10); wrap them once.
- **Environment variables:** implementation-specific ones use the `OSOE_` prefix.
  `SHMEM_` belongs to the specification.
- **Symbols:** `libosoe` exports only spec API names; internal non-static symbols
  use an `osoe_` prefix and stay hidden. `shmem.exports` must keep passing.
- **The design doc stays true.** When implementation forces a decision the doc does
  not make, or shows a doc decision is wrong, update the doc with the rationale in
  the same commit. Never diverge silently.
- **Blockers are raised, not worked around.** If a rule above or a design decision
  blocks progress, stop and bring it to the user with the options you see.
- **Verify, and report faithfully.** Claims about kernel or compiler behaviour get
  checked in the VM. If a test fails, say so with the output.
- **Leave `docs/reviews/` alone.** It is a committed record of past reviews; read
  it, but do not edit it.

## Scope

### Step A — the SHMEM↔transport boundary (design, then checkpoint)

Define the internal interface between the SHMEM layer (§4) and whatever carries
operations to other PEs, in the terms of `docs/reviews/archive/core-api.md` §8.3: **put,
get, read-modify-write (for AMOs), fence, wait (completion, for quiet and
pe_quiet), and notify (waking watchers, §4.9)**. Even though AM (§2–§3) will be the
only network implementation, the SHMEM layer calls this interface and never AM
directly, so a future RDMA backend could replace handlers without touching §4.

- Write it as an internal header (for example `shmem/src/transport.h`), addressed
  as `(pe, region, offset)` (§4.1, invariant 20), with completion and ordering
  semantics stated in comments.
- Add a short design section recording it — where it sits in §2 or §4 is your call
  — including what each operation guarantees and what the intranode backend does.
- **Checkpoint:** show the user the header and the doc section, and wait for
  approval before building on them.

Read `docs/reviews/archive/core-api.md` §1, §2, and §8 for this step only. It was written
against the design *before* its review revision (for example it still describes a
per-node progress pool and a per-address watch table), so where it disagrees with
the current design doc, the design doc wins.

### Step B — the first slice

**Done when two or more PEs on one node can initialize, find each other, allocate
symmetric memory, put and get, and finalize cleanly — in all four presets.**

1. **First commit: fix `tools/probe/probe.c`**, which uses implicit-order
   `atomic_store`/`atomic_load` (three call sites), to follow §3.16.
2. **A local launcher for tests** that starts L processes on this machine with the
   `OSOE_` bootstrap variables set, waits for them, and fails on timeout or nonzero
   exit. There is no Slurm in the VM, so no multi-PE test runs without it. Mirror
   the §6.1 table under the `OSOE_` prefix and record the exact variable names in
   §6.2.
3. **`base/`**, an internal static library linked into `libosoe`:
   - futex2 wait/wake wrappers taking absolute deadlines, plus a deadline helper;
   - the §5.4 epoch counter type whose only comparison is `wait_eq`;
   - cache-aligned (`_Alignas`) single-writer counter blocks (§3.14 cost rules);
   - `OSOE_` environment parsing, and the `SHMEM_SYMMETRIC_SIZE` grammar (§4.4, spec
     §8) with unit tests.
4. **Control segment and node-local rendezvous (§6.2):** the lead (`LOCALID == 0`)
   creates `/osoe.<job>.<step>.<node>` (mode 0600) with a versioned header and one
   slot per PE (rank, pid, heap object name, bootstrap phase); local PEs open it by
   name; heap names are removed once all have mapped; the control segment keeps its
   name until the last finalize. Leave room in the header for the §3.14 metric
   table, but the table itself is out of scope.
5. **Agent threads (§3.12):** one per PE, created with all asynchronous signals
   blocked. In this slice they only need to start, idle, and stop cleanly.
6. **Per-PE heaps (§4.1):** each a named shared-memory object; each process maps its
   own heap at a fixed base `B` with `MAP_FIXED_NOREPLACE` and its siblings'
   heaps elsewhere, recorded in `base_of`. `SHMEM_SYMMETRIC_SIZE` agreement across
   local PEs is checked.
7. **Lifecycle (§6.4):** `shmem_init`, `shmem_finalize`, `shmem_my_pe`,
   `shmem_n_pes`, `shmem_query_initialized`; reference count, once-gate, non-last
   finalize performs a barrier, fork children do not tear down.
8. **Intranode `shmem_barrier_all`** on the §5.4 counters with the single-node
   release path (§5.5), beginning with the §4.6 serializing store.
9. **`shmem_malloc` and `shmem_free`:** deterministic, metadata out of band, with
   the §4.4 edge semantics that apply to them (barrier on exit for malloc, on
   entry for free; size 0 returns NULL without a barrier).
10. **`shmem_putmem`, `shmem_getmem`, `shmem_ptr`** for local PEs through the
    transport boundary's intranode backend (`memcpy`). `shmem_ptr` returns `NULL`
    for anything not reachable locally.

A job whose environment describes more than one node fails at init with a clear
"not yet supported" message.

### Out of scope for this slice

Networking and io_uring; Slurm `SLURM_*` parsing (it arrives with the environment
fixtures of §8.2); AMOs, `wait_until`, signals; `shmem_quiet`, `shmem_fence`,
`shmem_pe_quiet` as API routines; other allocator routines; teams and contexts
beyond `SHMEM_TEAM_WORLD`/`SHMEM_CTX_DEFAULT` plumbing; `osoe-stat`, metrics, and
the flight recorder; the §7.4 mandatory surface. Note anything you find that
belongs to these for later rather than doing it.

## Testing

- Unit tests (tier 1) for pure logic: size grammar, allocator determinism (same
  call sequence → identical offsets), counter wait semantics.
- Multi-process tests through the launcher for L = 1, 2, and 4, covering init and
  finalize (including repeated init/finalize), barrier, malloc/free symmetry across
  PEs, put/get between every pair of local PEs, and `shmem_ptr`.
- Every test has a ctest timeout and a label (`tier1` or `tier2`).
- Every commit builds with no warnings and passes all tests in all four presets in
  the VM. ThreadSanitizer cannot see races across processes; say where that limits
  what `tsan` proves.

## Working method

- State a short plan first, then do Step A and stop at its checkpoint.
- After approval, work in small commits on a branch named `phase1-slice1`, each one
  building and passing in the VM, with messages that say what was verified. End
  commit messages with the attribution line your environment specifies.
- When the slice is done, summarize: commits, what was verified and how, design
  doc changes, anything deferred or found out of scope, and open questions. Ask
  before merging the branch into `main`.
