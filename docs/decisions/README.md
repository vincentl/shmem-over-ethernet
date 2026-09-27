# Decisions in force

One line a decision that still holds, grouped by what it governs. Each names
who took it and links the dated record that explains why and says how to undo
it. The dated files beside this one are the history; this page is what to read
to know the rules today. A decision that changes one of these lines updates it
here in the same commit, and a reversed one moves to the last section.

"Author" means the project's author decided; "Claude" means a session decided
without the author, under a standing mandate, and the record carries the undo.

## Scope and goals

- **Release point first.** 1.5 complete through step 8, 1.6 less its known gaps, the SOS suite clean of its own extensions, a green Pi runbook, a license, `cmake --install`, a README, a version tag. Author, [2026-09-26-day](2026-09-26-day.md) item 2.
- **RoCE is a constraint, not work.** The seams stay RDMA-capable; no RoCE backend before the release; Soft-RoCE at most after it. Author, [2026-09-26-day](2026-09-26-day.md) item 1.
- **Step 8 in three slices.** Contexts serial; then threads behind an interim per-context mutex; then a lock-free shared mode. Author, [2026-09-26-day](2026-09-26-day.md) item 3.
- **Scans modeled before built, right after step 8 slice 1.** Author, [2026-09-26-day](2026-09-26-day.md) item 6.
- **Scan design A.** Every member scans at its team rank's place in a pre-order k-ary tree, on its own thread. Author, [2026-09-26-evening](2026-09-26-evening.md) item 4.
- **collect's lengths through the reduction tree.** Per-member lengths travel through the tree's fixed per-team slots into private memory, never a symmetric heap borrow. Author, [2026-09-26-evening](2026-09-26-evening.md) item 6.
- **Team creation as proposed.** A runtime `OSOE_MAX_TEAMS`, per-team storage blocks, team numbers from one counter never reused. Author, [2026-09-25](2026-09-25.md) item 1.
- **Large collectives get both fixes.** A pipelined internode tree, then a dedicated internode allgather. Author, [2026-09-25](2026-09-25.md) item 2.
- **Small gathers keep Bruck's cost.** No two-slot staging and no crossover. Author, [2026-09-26-day](2026-09-26-day.md) item 5.
- **No strided primitive at the transport boundary.** `iput` batches puts; `iget` uses the nonblocking get. Author, [2026-09-24-overnight](2026-09-24-overnight.md), "Typed RMA".
- **Deprecated active-set collectives run on any active set**: the world team's path for the whole job, member-only algorithms over `pSync` for any other. Author, [2026-09-26-evening](2026-09-26-evening.md) item 13.
- **The deprecated routines' profiling names come in both readings**, `pstart_pes` (Sandia's) and `pshmem_start_pes` (literal). Author, [2026-09-26-evening](2026-09-26-evening.md) item 13.

## Protocols and data structures

- **Progress.** An agent thread in every PE; the local lead's agent alone drives the transport; no helper process. Design §3.12.
- **Atomics.** ISO C11 `stdatomic.h` with named orders only; no intrinsics, no assembly, no spin before blocking. Design §3.16.
- **Blocking.** One watch slot per local PE with a 32-bit futex sequence word. Design §4.9.
- **Collective epochs.** Cumulative 32-bit counters with equality waits; sense reversal retired. Design §5.4.
- **The internode seam.** A blocking call on the lead's thread carrying the operation and element, with a generic tree any backend may reuse; a backend's only routing input is the node map. Design §4.10, §5.8.
- **The agent's boundary call.** `osoe_transport_agent_wait` and `agent_exit` give a backend the agent thread and a last drain. Design §3.12, §4.10.
- **Introspection.** `osoe-stat` reads the control segment through `/proc/<pid>/fd`; the segment's name is removed at init. Design §3.14.
- **Job handshake.** An optional `OSOE_JOB_SECRET`, proved by a SipHash-2-4 challenge, never sent. Design §6.2.
- **Abort.** An abortive goodbye on every open connection, not a flood down the tree. Claude, confirmed by the author, [2026-09-23-autonomous](2026-09-23-autonomous.md) item 12.
- **The abort word.** One 64-bit word claimed by one CAS; the lead's agent re-wakes every local agent. Claude, [2026-09-23-autonomous](2026-09-23-autonomous.md) items 13-14.
- **Global exit.** Only the calling PE runs `exit(status)`; every other PE flushes and calls `_exit(status)`. Claude, [2026-09-24-overnight](2026-09-24-overnight.md) item 10.
- **A reset connection** is judged only once the peer's goodbye and end of stream have both arrived. Claude, model-checked, [2026-09-24-overnight](2026-09-24-overnight.md), "Question 10".
- **PE deaths** are found by pidfds each agent polls in its own sweep. Claude, confirmed by the author, [2026-09-24-overnight](2026-09-24-overnight.md) items 11-13.
- **No development watchdog.** The ending agent names the routine its PE was blocked in, in every build. Claude, confirmed by the author, [2026-09-24-overnight](2026-09-24-overnight.md) items 11-13.
- **Slurm's variables** are read only when `OSOE_PROCID` is unset, never mixed with osoe's. Claude, [2026-09-24-overnight](2026-09-24-overnight.md) item 4.
- **A host whose name resolves to loopback** binds and advertises the address it would use to reach a peer. Claude, [2026-09-24-overnight](2026-09-24-overnight.md) item 8.
- **Typed RMA covers every Table 5 type.** Diagnostics name the routine actually called and its element width. Claude, [2026-09-24-overnight](2026-09-24-overnight.md) item 16.
- **fcollect's lead gathers its own members' blocks;** a peer's buffer is written only after that peer has entered. Claude, [2026-09-24-day](2026-09-24-day.md) items 7-8.
- **The distributed lock** is an MCS queue lock homed by address hash, model-checked first-come first-served. Claude, [2026-09-24-day](2026-09-24-day.md) item 4.
- **Symmetric statics** are gathered by a linker script over the whole executable; `-no-pie` is required and `-static` refused. Claude, [2026-09-24-day](2026-09-24-day.md) item 5.
- **The pipelined internode tree** lets a lead take whichever ready input comes first. Claude, [2026-09-26-overnight](2026-09-26-overnight.md) item 1.
- **Internode allgather** runs Bruck's steps among node leads at the seam. Claude, [2026-09-26-overnight](2026-09-26-overnight.md) item 5.
- **A team's record carries its world ranks** as start, stride and member count; the upcall that computed them is gone. Author, [2026-09-26-day](2026-09-26-day.md) item 7; built in [2026-09-26-evening](2026-09-26-evening.md) item 5.
- **Contexts.** A pool of `OSOE_MAX_CONTEXTS`; a nonblocking get's token names its context; `NOSTORE` is recorded and still runs the ordering fences. Claude, [2026-09-26-evening](2026-09-26-evening.md) item 2.
- **Shared contexts issue without a lock**: a generation word and two counter pairs for gets, an atomic dirty bitmap with compare-and-swap highest segments for puts, and a mutex taken only by waits; a wait that finds a node behind after its clear re-sets the bit and moves on. Author (the design and the change to item 2), [2026-09-26-evening](2026-09-26-evening.md) items 10-11.
- **`OSOE_CHECK_QUIET` and `OSOE_TEST_PERTURB`** are test hooks kept in the library, off by default, and the threaded test runs with both in every preset: together they catch the race mutations the tests otherwise cannot see. Claude, recommended to the author, [2026-09-26-evening](2026-09-26-evening.md) item 12.
- **Team destroy** tears down its shareable contexts and is fatal, by name, on a live private one; finalize destroys what is left. Claude, [2026-09-26-evening](2026-09-26-evening.md) item 2.
- **Cross-node scans** use slots and counters per team number, never shared per-PE slots, so threads cannot race on them. Claude, [2026-09-26-evening](2026-09-26-evening.md) item 5.

## Settings and defaults

- **`OSOE_MAX_TEAMS`** defaults to 64, at most 1,024. Author, [2026-09-26-day](2026-09-26-day.md) item 4.
- **`OSOE_MAX_CONTEXTS`** defaults to 64, from 1 to 4,096, agreed by a node's PEs at start. Claude, [2026-09-26-evening](2026-09-26-evening.md) item 2.
- **`OSOE_MAX_THREADS`** counts application threads, default 16, at most 1,023; the agent's block is one more, and a thread past it ends the job by name. Claude, [2026-09-26-evening](2026-09-26-evening.md) item 9.
- **`shmem_init` gives `SERIALIZED`**; `shmem_init_thread` gives `MULTIPLE` when asked. Claude, [2026-09-26-evening](2026-09-26-evening.md) item 9.
- **`OSOE_COLLECTIVE_PIPELINE`** defaults to 2, from 1 to 8, agreed at rendezvous. [2026-09-26-overnight](2026-09-26-overnight.md) item 1.
- **Keepalive** is 60 s idle, 10 s between probes, 6 probes; `TCP_USER_TIMEOUT` is not set; `OSOE_KEEPALIVE` overrides. Claude, [2026-09-23-autonomous](2026-09-23-autonomous.md) item 17.
- **`OSOE_HEAP_BASE`** must be page-aligned, and an unaligned one is refused by name. Claude, [2026-09-24-overnight](2026-09-24-overnight.md) item 17.
- **The `/dev/shm` capacity check** refuses by default; `OSOE_SHM_CAPACITY_CHECK=0` makes it a report. Design §6.2.
- **Dependencies.** The distribution's liburing 2.6 or later, never a bundled copy; CMake 3.25 or later. Design §1.
- **Names.** The library is `libosoe` with hidden visibility; `OSOE_`/`osoe_` are internal, `SHMEM_` belongs to the specification. Design §1.

## How work is done

- **Merge as each slice verifies**, and keep going without waiting, with every decision written down with its undo. The author's standing direction, [2026-09-23-autonomous](2026-09-23-autonomous.md).
- **Verification before merge**, on the exact tree that merges: four presets, tier 3, mutations on a copy, the load loop, the SOS sweep, and an adversarial review whose findings are fixed first. [2026-09-24-day](2026-09-24-day.md).
- **A speed claim needs a timing simulation** beside its model. [2026-09-26-day](2026-09-26-day.md).
- **A subagent's report is evidence, not a verdict.** [2026-09-26-day](2026-09-26-day.md).
- **Model-checker scripts run once, in the release preset.** Claude, [2026-09-23-autonomous](2026-09-23-autonomous.md) item 9.
- **The costly THREAD_MULTIPLE models are opt-in** with `OSOE_MULTIPLE_MODELS=ON`. Claude, [2026-09-23-autonomous](2026-09-23-autonomous.md) item 10.
- **The VM is shared.** Long runs take the lock, measurements never overlap another session's, and runs are killed by PID. [2026-09-26-day](2026-09-26-day.md).
- **Slurm stays installed in the VM,** and fixtures come from real Slurm and the Pi cluster. [2026-09-24-overnight](2026-09-24-overnight.md) items 5-6.
- **Disk.** Merged worktrees are removed; uncommitted work is archived first. Author, [2026-09-26-overnight](2026-09-26-overnight.md) item 7.

## License and release

- **License: GNU LGPL, version 3 or any later version.** `COPYING` and `COPYING.LESSER`; `SPDX-License-Identifier: LGPL-3.0-or-later` on every source file. Author, [2026-09-26-evening](2026-09-26-evening.md) items 3 and 14.

## Reversed

- `OSOE_MAX_TEAMS` default 32, replaced by 64 ([2026-09-26-day](2026-09-26-day.md) item 4).
- The internode team-world-ranks upcall, replaced by the record's start and stride ([2026-09-26-day](2026-09-26-day.md) item 7).
- `OSOE_LIVENESS` and `OSOE_WATCHDOG`, removed with the watchdog ([2026-09-24-overnight](2026-09-24-overnight.md) items 11-13).
- The exit hand-off inside `exit()`, removed once only the caller exits ([2026-09-24-overnight](2026-09-24-overnight.md) item 10).
- An abort flooding down the tree, replaced by the abortive goodbye ([2026-09-23-autonomous](2026-09-23-autonomous.md) item 12).
- Slurm fixtures written from documentation, replaced by captured ones ([2026-09-24-day](2026-09-24-day.md)).
