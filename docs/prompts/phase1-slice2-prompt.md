# Implement phase 1, step 1, second slice: finish the single node

You are continuing implementation work on **osoe** (OpenSHMEM Over Ethernet).
Phase 1, step 1 — "one node, no network" (design §7.1) — is most of the way done.
This slice finishes it: signals, a development-only watchdog, introspection, and
the mechanical breadth that conformance needs.

## Context

- **Repository:** `/Users/vincent/Desktop/Maker Space/shmem-over-ethernet`.
- **Branch:** continue on **`phase1-slice2`**, which already carries three commits
  (the §4.9 watch-slot model, watch slots with `rmw` and `load` in the transport
  boundary and the `uint64`/`uint32` AMOs, and `shmem_wait_until`/`shmem_test`).
  `main` holds the first slice.
- **Design record:** `docs/design/shmem-ethernet-design.md`. Each section states a
  decision, its rationale, and rejected alternatives. §9 lists load-bearing
  invariants; §10 lists closed decisions and open questions.
- **Specification:** `docs/reference/OpenSHMEM-1.6.pdf` (local, gitignored;
  `pdftotext -layout` works on the host). Cite it as §9.x.y.
- **Linux environment:** the host is macOS. A Lima VM named **`shmem`** (Debian 13,
  kernel 6.12, aarch64, 4 CPUs) mounts the repository at the same absolute path.
  Run everything Linux-side from the repository root, for example
  `limactl shell shmem cmake --workflow --preset debug`. The presets refuse to run
  on macOS.

### What exists

- **`base/`** — futex2 wait/wake on 32-bit words with absolute deadlines and a
  timeout count, the §5.4 epoch counter whose only comparison is `wait_eq`,
  cache-aligned single-writer counter blocks, the serializing store, `OSOE_`
  environment parsing, and the `SHMEM_SYMMETRIC_SIZE` grammar.
- **`shmem/src/transport.h`** — the SHMEM↔transport boundary (§4.10): put, get,
  rmw, fence, wait, notify, a coherent load, the watch half, and `direct`. Its
  contract promises no more than the spec. **Nothing above it asks what a backend
  supports**, and backends are chosen at build time.
- **`transport_local.c`, `watch.c`** — the intranode backend and the §4.9 watch
  slots (one per PE, watching an aligned power-of-two block).
- **The control segment** `/osoe.<job>.<step>.<node>.<session>` with a versioned
  header (layout 2), one slot per PE, and the world team's epoch counters; per-PE
  heaps at a lead-chosen base `B`; agent threads; the §6.4 lifecycle.
- **API so far:** `shmem_init`, `shmem_finalize`, `shmem_query_initialized`,
  `shmem_my_pe`, `shmem_n_pes`, `shmem_barrier_all`, `shmem_malloc`, `shmem_free`,
  `shmem_putmem`, `shmem_getmem`, `shmem_ptr`, `shmem_info_get_*`, the
  `shmem_uint64_*` and `shmem_uint32_*` AMOs, `shmem_uint{64,32}_wait_until` and
  `_test`.
- **Tests:** 37 in tiers 1 and 2, every one with a ctest timeout and a label.
  `tools/launch/osoe-launch -n L -- program` runs a one-node job of L PEs and fails
  on a nonzero exit, a signal, a timeout, or leaked shared memory.
- **`tools/modelcheck/`** — the model checker. Develop models there; copy the model,
  `engine.py` and the recorded `.out` into `docs/design/evidence/<topic>/` when a
  design cites them.

**Read these design sections before planning:** §3.12 (agent threads), §3.14
(introspection and metrics, in full), §3.15 (error model), §3.16 (memory order),
§4.4 (allocator edge semantics), §4.6 (ordering and completion), §4.9
(point-to-point synchronization), §4.10 (the boundary), §5.6, §6.4, §7.1–§7.5,
§8.1–§8.2, §8.6, §9, §10.

## Rules

These are project decisions, not preferences.

- **Atomics (§3.16):** ISO C11 `<stdatomic.h>` only. Every access to a shared
  protocol word is an `atomic_*_explicit` call naming one of the four ISO orders:
  loads `acquire`; CAS and `fetch_add`/`fetch_sub` `seq_cst`; single-writer
  publication stores `release`; **both** stores of a two-sided protocol `seq_cst`.
  No implicit-order calls, no `memory_order_relaxed` on protocol words, no GCC
  extension orders, no compiler intrinsics, no `__builtin_*`, no
  `__atomic_*`/`__sync_*`, no assembly. Use C23 `<stdbit.h>` for bit operations.
  Futex words are `_Atomic uint32_t` with a `_Static_assert` on size and alignment.
- **Dependencies (§1):** libc and the distribution's liburing only.
- **Futex deadlines are absolute** (§3.10), and every library wait carries one.
- **Environment variables:** implementation-specific ones use the `OSOE_` prefix;
  `SHMEM_` belongs to the specification.
- **Symbols:** `libosoe` exports only spec API names — widening to `^p?shmem_` and
  `^shmemx_` as the profiling interface and `shmemx.h` arrive (§7.4). Internal
  non-static symbols use an `osoe_` prefix and stay hidden. `shmem.exports` must
  keep passing.
- **No capability queries above the boundary** (§4.10). Every operation is
  mandatory for every backend; a backend without native support emulates
  underneath.
- **The design doc stays true.** When implementation forces a decision the doc does
  not make, or shows a doc decision is wrong, update the doc with the rationale in
  the same commit. Never diverge silently.
- **Model-check before implementing** any new concurrent protocol, as §4.9 and
  §4.11 did. A model in which nothing can fail proves nothing: break every rule
  the design states and show the failure.
- **Blockers are raised, not worked around.** If a rule or a design decision blocks
  progress, stop and bring it to the user with the options you see.
- **Verify, and report faithfully.** Claims about kernel or compiler behaviour get
  checked in the VM. If a test fails, say so with the output. When a test asserts
  that something blocks, wakes, or is ordered, break the mechanism on purpose once
  and confirm the test fails.
- **Leave `docs/reviews/` alone.** It is a committed record; read it, do not edit it.

## Scope, in order

### 1. Signals (§9.8.3, §9.8.4, §9.11.15)

`shmem_putmem_signal`, `shmem_signal_set`, `shmem_signal_add`,
`shmem_signal_fetch`, and `shmem_signal_wait_until`, with `SHMEM_SIGNAL_SET` and
`SHMEM_SIGNAL_ADD`. Signal variables are `uint64_t` symmetric objects.

- **Put-with-signal is a put followed by an rmw carrying `OSOE_TP_FENCED`**, so the
  signal never announces bytes that have not landed (invariant 5). Do not add a
  compound transport operation.
- `shmem_signal_set` is a set-style update, so it is a `seq_cst` store against a
  waiting PE (§4.9, §3.16). `shmem_signal_fetch` is the boundary's coherent load of
  the caller's own signal.
- `shmem_signal_wait_until` is `wait_until` on a `uint64_t` signal; note in the doc
  that the spec requires it not to return before the update is complete.

### 2. A development-only watchdog (§3.12, §3.15)

Today a PE that blocks in `wait_until` or a barrier when a sibling has died waits
until the launcher's timeout. Give the developer a loud failure instead.

- **An abort word in the control segment header**, plus a liveness check of the
  local PEs' recorded pids, tested by every library wait at each safety-sweep
  deadline. On a dead PE or a set abort word, abort loudly, naming the PE, the
  routine, and the control segment.
- **It is a development aid, not the release path.** Production relies on the
  launcher tearing the job down (`srun -K1`, §3.15). So it is **on by default in
  the `debug`, `asan` and `tsan` presets and off in `release`**, with an `OSOE_`
  environment variable to force either way. Record that scoping and its reason in
  §3.15, and note what it does *not* do: no pidfd polling, no cross-node
  detection, no `shmem_global_exit` (§7.1 step 4).
- Keep the per-wait cost at zero when nothing is wrong: the check happens only on
  a sweep expiry, never on the wake path.

### 3. Introspection: the metric table and `osoe-stat` (§3.14)

**Checkpoint first.** Before writing the tool, show the user the metric-table
layout, the counter catalogue you propose (drawn from §3.14's "by the question each
metric answers"), and where each counter is written, and wait for approval.

- **The self-describing metric table** in the control segment: name, type, unit and
  offset per metric, checksummed, with the layout version bumped. `osoe-stat` must
  work against a job built from a different commit.
- **Counter blocks** per thread (§3.14 cost rules): one per application thread and
  per agent, cache-line aligned, single-writer.
- **A "blocked in" record** per PE, so a hung job says where each PE is waiting,
  and a heartbeat.
- **`osoe-stat`**, an installed tool that maps the segment read-only and needs no
  cooperation from the job: a live counter view, a state dump, human-readable and
  JSON output, and `--gc` for segments left by crashed jobs.
- The flight recorder may wait; say so in the doc if you defer it.

### 4. Mechanical breadth

Each of these is expansion of a decision already made. Group them into commits as
you see fit.

- **`shmem_quiet`, `shmem_fence`, `shmem_pe_quiet`** as API routines over the
  boundary's fence and wait (§4.6). `pe_quiet` with `npes == 0` returns at once.
- **`shmem_calloc`, `shmem_realloc`, `shmem_align`, `shmem_malloc_with_hints`**
  with the §4.4 edge semantics, including `calloc`'s overflow rule and `realloc`'s
  hint preservation.
- **The rest of the AMO and wait types** across Tables 6–8 for 4- and 8-byte
  widths, plus the C11 type-generic interfaces (§7.4).
- **The multiple-element waits and tests** (§9.11.2–§9.11.14). The watch slot
  already covers a range, which is why it was designed that way. Mind the empty-set
  return values of 1.6 change #19.
- **`shmem_init_thread`, `shmem_query_thread`** providing up to
  `SHMEM_THREAD_SERIALIZED` (§4.6).
- **Team and context plumbing (§7.3):** a context handle on every RMA entry point
  and a team parameter on every collective, implementing only
  `SHMEM_CTX_DEFAULT`, `SHMEM_TEAM_WORLD` and `SHMEM_TEAM_SHARED`, plus
  `shmem_team_ptr`, `shmem_team_my_pe`, `shmem_team_n_pes`,
  `shmem_team_translate_pe`. `shmem_ctx_create` and the team splits return nonzero.
- **The §7.4 mandatory surface:** `shmemx.h`, the `pshmem_` profiling interface as
  weak aliases with `pshmem.h` and `shmem_pcontrol`, the deprecated routines and
  `mpp/shmem.h`, `SHMEM_DEBUG`/`SHMEM_VERSION`/`SHMEM_INFO` and the `SMA_*` forms,
  `shmem_pe_accessible`/`shmem_addr_accessible`, and `shmem_ctx_session_start`/`_stop`
  as no-ops. Widen `shmem.exports` accordingly.

### Out of scope for this slice

Networking and io_uring; Slurm `SLURM_*` parsing and the §8.2 environment
fixtures; `shmem_global_exit` and abort propagation; pidfd liveness and keepalive;
symmetric statics and the compiler wrapper; collectives beyond `barrier_all` and
`sync`; nonblocking RMA and AMOs; locks; `SHMEM_THREAD_MULTIPLE` (§4.11); the
heap-base-shift test mode. Note anything you find that belongs to these for later
rather than doing it.

## Testing

- **Tier 1** for pure logic: any new parsing or bit manipulation, and a model run
  for every new protocol.
- **Tier 2** through the launcher at L = 1, 2 and 4: producer-consumer over
  put-with-signal and `signal_wait_until`; `signal_add` and `signal_set` racing a
  waiter; the watchdog catching a `SIGKILL`ed PE loudly instead of hanging;
  `osoe-stat` against a live job and against a deliberately stalled one; the new
  allocator routines' edge semantics; the multiple-element waits; and the
  mandatory-surface entry points being present and callable.
- **Every test has a ctest timeout and a label** (`tier1` or `tier2`).
- **Every commit builds with no warnings and passes every test in all four presets
  in the VM.** Say where ThreadSanitizer's single-process view limits what it
  proves (§8.2).
- Timing checks must not depend on scheduling: use the arrival-table technique of
  `shmem/tests/arrivals.h`, not a per-PE clock reading.

## Working method

- State a short plan first. Stop at the §3.14 checkpoint and wait for approval.
- Work in small commits on `phase1-slice2`, each one building and passing in the
  VM, with messages that say what was verified and how. End commit messages with
  the attribution line your environment specifies.
- When the slice is done, summarize: commits, what was verified and how, design doc
  changes, anything deferred or found out of scope, and open questions. Ask before
  merging into `main`.
