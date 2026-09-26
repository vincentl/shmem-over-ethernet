# Implement phase 1, step 2, first slice: the in-process AM backend

You are starting **§7.1 step 2** of **osoe** (OpenSHMEM Over Ethernet). Step 1 —
"one node, no network" — is complete and merged. This step builds the second
backend behind the §4.10 boundary and validates every internode semantic
*before a socket exists*.

Read `docs/prompts/README.md` for what "phase", "step" and "slice" mean here;
they are three different numberings and only one is the unit of work.

## Setting up on a new machine

The repository is self-contained except for two things git does not carry.

**1. Clone, and check out `main`.** Everything below assumes the repository
root as the working directory.

**2. The development VM (macOS host).** io_uring and futex2 are Linux-only, so
the build and test loop runs in a Lima VM defined in the repository
(`dev/lima/osoe.yaml`, design §8.5):

```sh
brew install lima
dev/lima/create.sh              # creates and starts an instance named "osoe"
```

`create.sh` mounts the repository **writable at the same absolute path it has
on the host**, using `git rev-parse --show-toplevel`, so it follows wherever
you cloned to — you do not need to reproduce the original machine's path. That
same-path mount is what makes `limactl shell osoe …` from the repository root
start in the right directory, and what keeps paths in `build/*/compile_commands.json`
valid on both sides.

The instance name is just an argument: `dev/lima/create.sh <name>`. The default
is `osoe` and §8.5 uses that. **The earlier prompts in this directory say
`shmem`, because the original machine's instance was created with that name** —
substitute whichever you have. Every command below writes `osoe`.

First build on a new machine:

```sh
limactl shell osoe cmake --workflow --preset debug
```

**Run `tools/probe` before debugging anything else on a new machine** (§8.5).
It is registered as the ctest `env.probe` test and checks every kernel
assumption the design makes — single-issuer rings with an explicit CQ size,
`IORING_OP_FUTEX_WAIT` on a stale value, multishot recv, cross-process futexes,
and the absence of 64-bit futexes. If it fails, nothing else will work there.

**3. The specification PDF.** `docs/reference/*.pdf` is gitignored and not
redistributed. Download *OpenSHMEM Application Programming Interface, Version
1.6 (1 November 2024)* from <http://www.openshmem.org/> into `docs/reference/`.
On the macOS host, `pdftotext -layout` gives a readable text dump; section
numbers of the form §9.x.y throughout the design notes and these prompts refer
to it.

**4. The reading corpus (optional).** `/corpus/` is gitignored — public
OpenSHMEM code bases fetched for reading only, never built. `tools/corpus/fetch.sh`
re-fetches them against `tools/corpus/manifest.tsv` if you want them; nothing in
the build or the tests needs them.

Four presets must be green before you start: `debug`, `release`, `asan`, `tsan`.

## State

`main`, clean tree, **82 tests**, all four presets green. Step 1 is merged
(`5955d20`, then `4e96700` for its follow-ups).

What exists:

- **`base/`** — futex2 wait/wake on 32-bit words with absolute deadlines, the
  §5.4 epoch counter, cache-aligned single-writer counter blocks, the
  serializing store, `OSOE_` environment parsing, the `SHMEM_SYMMETRIC_SIZE`
  grammar.
- **`shmem/src/transport.h`** — the §4.10 boundary: put, get, rmw, fence, wait,
  notify, a coherent load, the watch half, and `direct`. **Nothing above it asks
  what a backend supports**, and backends are chosen at build time.
- **`transport_local.c`** — the intranode backend, and your reference
  implementation. Every operation is a memory operation in the calling thread,
  applied before it returns; nothing is ever outstanding.
- **The control segment** (layout 5) with the §3.14 metric table, per-thread
  counter blocks, the "blocked in" record, and per-team epoch counters; per-PE
  heaps at a lead-chosen base `B`; agent threads; the §6.4 lifecycle.
- **`osoe-stat`** — an installed tool that maps the control segment read-only
  and needs no cooperation from the job. Written against the *segment*, not the
  library: it includes nothing from `shmem/src` and finds every value by name
  through the metric table.
- **The API surface of step 1**: 526 routines, each a strong `pshmem_`
  definition with a weak `shmem_` alias. Teams and contexts are plumbed on every
  entry point with `SHMEM_TEAM_WORLD`, `SHMEM_TEAM_SHARED` and
  `SHMEM_CTX_DEFAULT` implemented.
- **`tools/modelcheck/`** — the model checker. Develop models there; copy the
  model, `engine.py` and the recorded `.out` into `docs/design/evidence/<topic>/`
  when a design cites them.

**Read these design sections before planning:** §3.1–§3.13 (the AM layer, in
full — this step implements most of it), §3.14 (what the new counters must look
like), §3.16, §4.6 (ordering and completion, especially the dirty set), §4.10
(the boundary's contract, in full), §5.1–§5.6 (the two-level collectives), §8.1
(the logical node map and the force-remote flag), §8.2, §9, §10.

## Scope

§7.1 step 2, as the design states it:

> **In-process AM backend (tier 1), slow mode first.** Internode put, get, and
> AMOs with landing zones and reply credits; `quiet`, `fence`, and `pe_quiet`
> with ack-only segments; `put_signal`; internode `barrier_all`. Semantics are
> validated before a socket exists.

"In-process" means the AM layer's queues are in memory rather than on sockets —
the same code path, with the wire replaced. "Slow mode first" is §3.14's policy
setting: no coalescing, one message per operation, synchronous acknowledgement.
Every test then runs both ways and the results are compared.

**This is the boundary's first real test.** It is the second backend behind
§4.10, so §8.1's **force-remote flag** can run the same program over both paths
and diff — the strongest differential test available, and the answer to "is the
boundary really carrying two backends" that is *running code* rather than
analysis. The intranode path is simple enough to trust as the reference.

### Two open items are timed to land with this step

They are scheduled here because this step writes the code that would otherwise
have to be restructured for them. Do not defer them past it.

1. **Decide `osoe_tp_sync`** (§10, "Still open — the RoCE exercise"). Does a
   collective's internode phase cross the boundary? The proposal is a mandatory
   `osoe_tp_sync(ctx, group)` over a group of *nodes*. Decide it **while writing
   the internode phase that is its only caller and its only evidence**, not
   before.
2. **`OSOE_TP_MORE`'s first caller** (§4.10). This is the first backend for which
   deferring a submission means anything. Mind the rule the original proposal
   missed and §4.10 now states: *a backend may defer an operation only if it owns
   a copy of the source bytes*, because put's contract is unconditional.

**`osoe_tp_set_range` also lands here** (§4.10), with the allocator call site it
needs — `shmem_malloc_with_hints` already records hints per allocation and grows
hinted allocations from the opposite end of the heap, precisely so this had a
call site when it arrived (§4.4).

### Out of scope for this step

Real sockets and io_uring submission (step 3); pidfd liveness, abort propagation
and `shmem_global_exit` (step 4); symmetric statics and the compiler wrapper
(§4.3 phase 1.5, after step 4); collectives beyond `barrier_all` and `sync`;
nonblocking RMA and AMOs; locks; `SHMEM_THREAD_MULTIPLE` (§4.11); the typed RMA
cross product (§7.5). Note anything you find that belongs to these for later
rather than doing it.

The **flight recorder** (§3.14) becomes implementable at this step, because its
events are transport events and they now exist. It is not required by this
slice; if you add it, add it with the events, and if you defer it, say so.

## Rules

These are project decisions, not preferences. They are unchanged from step 1 and
every one of them earned its place.

- **Atomics (§3.16):** ISO C11 `<stdatomic.h>` only. Every access to a shared
  protocol word is an `atomic_*_explicit` call naming one of the four ISO
  orders: loads `acquire`; CAS and `fetch_add`/`fetch_sub` `seq_cst`;
  single-writer publication stores `release`; **both** stores of a two-sided
  protocol `seq_cst`. No implicit-order calls, no `memory_order_relaxed` on
  protocol words, no GCC extension orders, no compiler intrinsics, no
  `__builtin_*`, no `__atomic_*`/`__sync_*`, no assembly. C23 `<stdbit.h>` for
  bit operations. Futex words are `_Atomic uint32_t` with a `_Static_assert` on
  size and alignment.
- **Dependencies (§1):** libc and the distribution's liburing only.
- **Futex deadlines are absolute** (§3.10), and every library wait carries one.
  Every blocking call sweeps at least every `OSOE_SWEEP_NS` (§4.10) — a backend
  that blocks without sweeping turns a clean abort into a hang.
- **Environment variables:** implementation-specific ones use `OSOE_`; `SHMEM_`
  belongs to the specification.
- **Symbols:** `libosoe` exports only `^p?shmem_`, `^shmemx_`, and the seven
  unprefixed deprecated routines. Internal non-static symbols use an `osoe_`
  prefix and stay hidden. Every public routine is a strong `pshmem_` definition
  with a weak `shmem_` alias, and the library's own internal calls go to the
  `pshmem_` name. `shmem.exports` and `shmem.profiling_aliases` must keep
  passing.
- **No capability queries above the boundary** (§4.10). Every operation is
  mandatory for every backend; a backend without native support emulates
  underneath.
- **The design doc stays true.** When implementation forces a decision the doc
  does not make, or shows a doc decision is wrong, update the doc with the
  rationale **in the same commit**. Never diverge silently.
- **Model-check before implementing any new concurrent protocol.** A model in
  which nothing can fail proves nothing: break every rule the design states and
  show the failure. §3.5's reservation protocol and §3.8's deferred reply queues
  both need this.
- **When a test asserts that something blocks, wakes, or is ordered, break the
  mechanism on purpose once and confirm the test fails.** Record it in the
  commit message. In step 1 this found three tests that passed while asserting
  nothing; expect it to keep earning its place.
- **Every commit builds with no warnings and passes every test in all four
  presets in the VM.**
- **Blockers are raised, not worked around.** If a rule or a design decision
  blocks progress, stop and bring it to the user with the options you see.
- **Leave `docs/reviews/` alone.** It is a committed record: read it, do not
  edit it.

## Testing

- **Tier 1** for pure logic: every new parsing and bit manipulation, and a model
  run for every new protocol.
- **Tier 2** through `osoe-launch` for the in-process AM backend, over §8.1's
  four poles of the logical node map: all PEs in one logical node; one PE per
  logical node; uneven node sizes (`0,0,0,1,2,2`); and **ranks not contiguous
  per node** (`0,1,2,0,1,0`), the shape Slurm's cyclic distribution produces.
- **The differential test**: the same program over the intranode path and the
  force-remote path, results diffed. Make this cheap to run early; it is the
  reason this step exists in this order.
- **Every test has a ctest timeout and a label** (`tier1` or `tier2`).
- Every tier-2 test calls `shmem/tests/invariants.h` before its last
  `shmem_finalize` — §8.2's rule that every `invariant.*` counter is zero at
  quiescence. New invariant counters are picked up automatically: it finds them
  by *kind* through the metric table, not by name.
- Say where ThreadSanitizer's single-process view limits what it proves (§8.2).
- Timing checks must not depend on scheduling: use the arrival-table technique
  of `shmem/tests/arrivals.h`, not a per-PE clock reading.
- Counting a routine's calls out of the control segment (§3.14) is how step 1
  tested things that have no observable value on a backend where nothing is
  outstanding — the barrier rules of §4.4, `quiet` versus `fence`, `sync` versus
  `barrier_all`. That technique is available and should be reached for.

## Working method

- **State a short plan first, and stop for approval before writing the AM
  layer's data structures.** They are the step's load-bearing decision.
- Work in small commits on a branch named `phase1-step2-slice1`, each one
  building and passing in the VM, with messages that say what was verified and
  how.
- Give the user periodic feedback as you go, not only at the end.
- End commit messages with the attribution line your environment specifies.
- When the slice is done, summarize: commits, what was verified and how, design
  doc changes, anything deferred or found out of scope, and open questions.
  **Ask before merging into `main`.**
