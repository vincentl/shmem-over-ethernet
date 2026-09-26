# Finish phase 1, step 1, second slice

You are continuing implementation work on **osoe** (OpenSHMEM Over Ethernet) on
branch `phase1-slice2`. A previous session completed roughly the first third of
the slice. **Read `docs/prompts/done/phase1-step1-slice2-prompt.md` first** — it is the
slice's brief, and every rule in it still applies unchanged. This document says
what has been done since, what was decided and why, and what is left.

## State

Branch `phase1-slice2`, clean tree, 47 tests, **all four presets green**
(`limactl shell shmem cmake --workflow --preset debug|release|asan|tsan`).

Commits since the slice began, newest first:

| Commit | What |
|---|---|
| `c34ead2` | §4.9: what waiting on many costs, and the two options not taken |
| `5b782fb` | The metric table, counter blocks, and the "blocked in" record |
| `4cf2a93` | Analysis: implementing the transport boundary on RoCE |
| `2e44e7f` | A boundary that carries RDMA, from the RoCE exercise |
| `31a890a` | The watch word publishes a range, not a power-of-two block |
| `b0873d6` | Analysis: coverage predicates for the §4.9 watch slot |
| `039bd42` | A development-only watchdog for a PE that dies while others wait |
| `aeb8d91` | Signalling operations over the boundary's fenced rmw |

Read those commit messages. They carry the reasoning, and several record
measurements you should not repeat.

## What is done, from the original scope

- **Signals (scope item 1)** — complete. `shmem_putmem_signal` as a put followed
  by an rmw carrying `OSOE_TP_FENCED`, `shmem_signal_set`/`_add`/`_fetch`/
  `_wait_until`, `SHMEM_SIGNAL_SET`/`ADD`. Only `putmem_signal`, not the typed
  families: the typed RMA cross product is deferred (§7.5) and put_signal stays
  consistent with put.
- **The development watchdog (scope item 2)** — complete. Abort word in the
  control-segment header plus a `kill(pid, 0)` liveness check, run **only** when
  a library wait's safety-sweep deadline expires, never on the wake path. On in
  debug/asan/tsan, off in release, `OSOE_WATCHDOG=0|1` forces either way.
  `osoe-launch` gained `-k`, which leaves survivors alone when one process
  fails, so a test can show the library ending the job rather than the launcher.
- **Introspection (scope item 3)** — **half done.** The writer side is in: the
  metric table, the frozen directory, counter blocks, the "blocked in" record,
  and the instrumentation. **The reader side is not**: see "Do this first".

## Decisions taken this session that are not in the original prompt

All are folded into `docs/design/shmem-ethernet-design.md`. Do not reopen them
from scratch; if you disagree, raise it.

1. **The §4.9 watch word changed encoding.** It was the smallest aligned
   power-of-two block containing the range, which had to contain *both ends* and
   so inflated an 8 KiB array wait to 122 KiB on average — measured, with 35.7%
   spurious coverage under the adjacent-write pattern of a halo exchange. It is
   now §4.11 item 3's word: region, first grain, saturating grain count, 4-byte
   grain (`shmem/src/watch_range.h`). §4.9 and §4.11 had specified *different*
   encodings; they are now one. The old one also had two latent defects — a
   shift of 4294967295 in its saturating case, confirmed under UBSan, and a loop
   that did not terminate when a range's ends differed in bit 63.
2. **A RoCE exercise was reviewed and folded in.** `docs/reviews/archive/roce-transport.md`
   is the analysis. The boundary carries RDMA; of six losses, four are RoCE's own
   or a phase boundary. §4.10 gained: dispatch per `(pe, operation class)`; every
   blocking call sweeps at least every `OSOE_SWEEP_NS`; `OSOE_TP_MORE` specified
   *with the rule the proposal missed* — a backend may defer only if it owns a
   copy of the source bytes; nonblocking completion is counters, never handles;
   and `set_range`'s contract. **`osoe_tp_sync` and `osoe_tp_set_range` are
   deliberately not declared** — the rule held to was that design-doc amendments
   are free but a `transport.h` declaration needs an implementation and a caller.
   They are scheduled into **§7.1 step 2 and step 3** respectively so they are
   work items, not notes.
3. **§4.4 gained two corrections** you must implement: `shmem_realloc` barriers
   on **entry and exit**, not exit alone (it is free and malloc in one call); and
   hints are **recorded, not discarded**, because spec §9.3.3 requires `realloc`
   to preserve them and to return NULL rather than migrate a block whose hints
   cannot be kept.
4. **`SHMEM_SYMMETRIC_SIZE` is now bounded** at what the watch encoding
   addresses (4 TiB), refused loudly at init. It was unbounded.
5. **`OSOE_NODE_LAYOUT` is 5**, and §10 records that it covers the *encoding* of
   every word in the segment, not only the offsets of fields.
6. **Waiting on many is left simple** (§4.9). Watch the span of the **unmasked**
   elements, which is free because the wait already walks `status`. The futex
   bitmask and `futex_waitv` are recorded as options and rejected: no public
   users of these routines at all, and the idiom the spec demonstrates is the
   non-blocking `shmem_test_some`, which touches no watch slot.

## Do this first: the reader side of §3.14

The metric table is written, checksummed and compared across PEs, and counters
are being incremented — but **nothing reads the segment**, so two claims are
currently untested:

- that a tool built from a different commit reads a job correctly;
- that every `invariant.*` counter is zero at quiescence (§8.2), which nothing
  asserts, so `invariant.missed_wake` could be firing on every run unnoticed.

Build these together, because the second is the first real test of the first.

**`osoe-stat`**, an installed tool (note `tools/` currently says "never
installed" — that changes for this one), which:

- maps the control segment **read-only**, needs no cooperation from the job, and
  works as well on a hung job as a running one;
- is written **against the segment, not the library**. It must include nothing
  from `shmem/src`. It reads the frozen `struct osoe_segment_directory`, checks
  the magic, then finds every value through the metric table **by name**. The
  directory is the only thing it may know by layout — that is what makes
  "self-describing" a mechanism rather than a claim, and the only way to keep it
  honest is for the reader to have no other way to find anything;
- offers a live counter view, a state dump (every slot, watch, epoch counter and
  "blocked in" record), human-readable and JSON output, and `--gc` for segments
  left by crashed jobs;
- reads per-thread CPU time and involuntary context switches from
  `/proc/<pid>/task/<tid>/`, using the tid each block records — §3.14's rule that
  anything the kernel already tracks is read by the tool, not maintained by the
  job. Netlink and Prometheus output are deferred.

The "blocked in" record is a generation-word protocol: load it, read the fields,
load it again, retry on odd-or-changed. `docs/design/evidence/blocked-record/`
has the model and the four ways to get it wrong.

**`shmem/tests/invariants.h`**, which every tier-2 test calls before
`shmem_finalize`: map the control segment read-only by name, walk the metric
table, assert every `OSOE_MKIND_INVARIANT` metric is zero across every block.
Reconstruct the name from `OSOE_JOB_ID`, `OSOE_STEP_ID`, `OSOE_NODEID` and the
session number (0 for a single init/finalize pair). Sharing the reader code with
`osoe-stat` is good; sharing it with the library is not.

Tier-2 tests required by the brief: `osoe-stat` against a live job, and against
one deliberately stalled, checking the heartbeat and the "blocked in" records.

## Then the rest of the slice, in the brief's order

6. `shmem_quiet`, `shmem_fence`, `shmem_pe_quiet` over the boundary's fence and
   wait. `pe_quiet` with `npes == 0` returns at once.
7. `shmem_calloc`, `shmem_realloc`, `shmem_align`, `shmem_malloc_with_hints`
   with §4.4's edge semantics — **including the two corrections above**.
8. The rest of the AMO types across Tables 6–8 at 4 and 8 bytes, plus the C11
   type-generic interfaces. Table 7 adds `float` and `double`, which are a
   bit-pattern move through the same rmw.
9. The wait and test types, the multiple-element waits and tests (§9.11.2–14)
   with the unmasked-span refinement, **and** the deprecated 2-byte
   `shmem_{short,ushort}_{wait_until,test}` plus `shmem_wait` — the user chose to
   include these; the boundary's `load` already handles width 2. Mind the
   empty-set return values of 1.6 change #19.
10. `shmem_init_thread`, `shmem_query_thread` providing up to
    `SHMEM_THREAD_SERIALIZED`.
11. Team and context plumbing (§7.3): a context handle on every RMA entry point
    and a team parameter on every collective, implementing only
    `SHMEM_CTX_DEFAULT`, `SHMEM_TEAM_WORLD` and `SHMEM_TEAM_SHARED`, plus
    `shmem_team_ptr`, `_my_pe`, `_n_pes`, `_translate_pe`. `shmem_ctx_create`
    and the team splits return nonzero. `shmem_team_sync`/`shmem_sync_all` are in
    scope; §5.6 says `barrier_all` includes a quiet and `sync` does not.
12. The §7.4 mandatory surface: `shmemx.h`; the `pshmem_` profiling interface as
    weak aliases with `pshmem.h` and `shmem_pcontrol`; the deprecated routines
    and `mpp/shmem.h`; `SHMEM_DEBUG`/`VERSION`/`INFO` and the `SMA_*` forms;
    `shmem_pe_accessible`/`shmem_addr_accessible`; `shmem_ctx_session_start`/
    `_stop` as no-ops. Widen `shmem.exports` to `^p?shmem_` and `^shmemx_`.
    The user chose: define the deprecated active-set collective constants
    (`SHMEM_SYNC_VALUE`, the pSync sizes) but **not** the active-set routines,
    which need machinery that does not exist.

    For the profiling flip, the MPI convention: `pshmem_X` is the strong
    definition and `shmem_X` a weak alias, so an application can interpose. A
    macro doing both in one line keeps the churn small, since the AMO and wait
    families are already macro-generated.

## Rules, unchanged and load-bearing

Everything in `docs/prompts/done/phase1-step1-slice2-prompt.md` still applies. The ones
most easily lost:

- **C11 `<stdatomic.h>` only**, named ISO orders: loads `acquire`, CAS and
  `fetch_add` `seq_cst`, single-writer publication stores `release`, **both**
  stores of a two-sided protocol `seq_cst`. No `relaxed` on protocol words, no
  intrinsics, no assembly.
- **Model-check before implementing any new concurrent protocol**, and break
  every rule the design states to show the failure. A model in which nothing can
  fail proves nothing. `tools/modelcheck/` has the engine; copy the model,
  `engine.py` and the recorded `.out` into `docs/design/evidence/<topic>/`.
- **When a test asserts that something blocks, wakes, or is ordered, break the
  mechanism on purpose once and confirm the test fails.** Every such check so
  far is recorded in its commit message; keep doing it.
- **Every commit builds with no warnings and passes every test in all four
  presets in the VM**, and updates the design doc in the same commit when it
  forces or contradicts a decision.
- **Blockers are raised, not worked around.**
- `docs/reviews/` is a committed record: read it, do not edit it. (Two files
  there are new this session and *were* corrected before committing —
  `roce-transport.md` and `watch-notification-options.md` — but they are
  committed now and the rule applies.)
- The VM is named `shmem` and mounts the repo at the same absolute path.

## When the slice is done

Summarize: commits, what was verified and how, design-doc changes, anything
deferred or found out of scope, and open questions. **Ask before merging into
`main`.**
