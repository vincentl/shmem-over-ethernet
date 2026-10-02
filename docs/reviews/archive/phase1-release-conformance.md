# Round three, question 1: is osoe OpenSHMEM 1.6?

Review of `main` at `b0e0b53`, run 2026-09-28 against
`docs/reviews/adversarial-review-prompt.md`. Two investigators read the
specification's text (`docs/reference/OpenSHMEM-1.6.pdf`, as
`pdftotext -layout`) family by family and checked the code against it, not
against the design record's summary: `conformance-a` (§9.1 setup and query,
§9.2 thread levels, §9.3 memory, §9.6 RMA, §9.7 AMO, §9.8 signals, §9.9
sessions, §9.11 point-to-point synchronization, §9.12 ordering, §9.13 locks,
§8 environment, Annexes C and F, the headers) and `conformance-b` (§9.4 teams,
§9.5 contexts, §9.10 collectives and scans, Annex F's active-set collectives,
`SHMEM_THREAD_MULTIPLE`). Where text and code disagreed or could not be told
apart, a program was written, built with the worktree's `oshcc` and run under
`osoe-launch` in the VM on one node and on logical node maps, and under the
VM's Slurm where the launcher mattered. Their programs are under
`build/scratch/review-r3/conformance-a/` and `conformance-b/`. Section numbers
are the PDF's. Labels: *documented*, *observed in source*, *measured*,
*recalled*; "lead-verified" means the lead re-read the cited lines.

## Verdict

**Yes, with two gaps in what the specification requires and a README that
under-reports its departures.** Across every routine family, neither
investigator found a defined behaviour implemented wrongly. The names are all
there (1,398 routines in the first half's families, every typed, sized,
context and `_nbi` form, checked by a generator against `nm -D` that was first
shown to catch deleted names; 1,787 weak `shmem_` over strong `pshmem_`
pairs); every C11 generic but one dispatches to a routine of the argument's
own type under `-pedantic-errors -Werror`; atomics from node-mates and from
other nodes interleave exactly on one word; fence, quiet, `pe_quiet`,
put-with-signal and nonblocking gets past the landing zone hold across nodes
as seen by the target and by a third PE; the type tables of the reductions
and scans equal Tables 10 and 11; the 1.6 broadcast writes the root's `dest`
and Annex F's does not; every argument rule of `split_strided` and `split_2d`
holds, negative strides included; and collectives run from several threads
on different teams at once, which no test anywhere exercises, work.

The two gaps: the C11 four-argument forms of `shmem_signal_add` and
`shmem_signal_set` that §9.8.5 and §9.8.7 list do not compile (C1); and under
the launch line the README recommends, `srun -K1`, a `shmem_global_exit` with
a nonzero status kills the calling PE during the normal termination §9.1.6
requires of it (C2). The README's statement of the thread limit is wrong (C3),
and its Limits section, which claims to list "every limit a program can
meet", is missing eight (C4). §10's still-blocking item, "a thorough 1.6
read", can be closed for the C interface once C1 is fixed; what this round did
not read end to end is in "What was not read".

## Findings, ranked

### C1. The C11 context forms of `shmem_signal_add` and `shmem_signal_set` do not compile (fix before release; high; *measured*, lead-verified in the header)

- **Location**: `shmem/include/shmem.h:182-185` declares
  `shmem_signal_set(uint64_t *, uint64_t, int)`, `shmem_signal_add(...)`,
  `shmem_ctx_signal_set(shmem_ctx_t, ...)` and `shmem_ctx_signal_add(...)`,
  with no generic over them. Spec §9.8.5 (`spec-1.6.txt:5711`) and §9.8.7
  (`:5776`) give the C11 synopsis
  `void shmem_signal_add(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal, int pe);`
  beside the three-argument C form, as every other context-taking routine's
  C11 generic is selected by argument count.
- **Scenario**: a C11 program written from the synopsis calls
  `shmem_signal_add(ctx, &sig, 1, pe)`. gcc: "passing argument 1 of
  'shmem_signal_add' from incompatible pointer type"; clang: "too many
  arguments to function call, expected 3, have 4".
- **Evidence**: *measured* (`conformance-a/generic-signal-ctx.c`, gcc and clang,
  `-std=c11`); *measured* that it is the only gap: every C11 synopsis in the
  specification, with and without `ctx`, over every type of Tables 5 to 8 and
  the deprecated generics, compiles with type-exact dispatch
  (`generic-ok.c`, `-std=c11 -pedantic-errors -Werror`). No test in either
  suite calls the four-argument form; SOS has only `shmemx_signal_add`. The
  README's "Everything the specification requires of a C implementation is
  here" is false by these two.
- **Severity**: fix before release: an arity selector in `shmem.h` in the
  style of `shmem_atomic_add`'s, and a compile test.

### C2. Under `srun -K1`, the README's recommended launch, a `shmem_global_exit` caller is killed during its normal termination (fix before release; high on the behaviour; *measured*)

- **Location**: `shmem/src/abort.c` (`osoe_abort_end`: the caller runs
  `exit(status)`; every other PE `fflush(NULL); _exit(status)` at once; design
  §3.15 "As built: every PE ends when its node aborts"); README "Running a
  program with `srun`" (`-K1`) and "How a job ends" ("the calling PE with
  `exit`, so its `atexit` handlers run and its streams are flushed, as the
  specification asks"); spec §9.1.6: "flushes I/O and releases resources in
  accordance with C/C++ language requirements for normal program termination"
  and "the exit status returned to the environment shall be one of the values
  passed".
- **Scenario**: PE 0 registers an `atexit` handler that takes 300 ms (a
  profiler writing its report, a program flushing results), prints to a piped
  stdout and calls `shmem_global_exit(7)`. The other PEs `_exit(7)` within
  milliseconds; Slurm's KillOnBadExit sees a nonzero exit and terminates the
  step; PE 0 is killed by SIGTERM inside `exit`; its handler never finishes,
  its buffered stdout is lost, and `srun` returns 143.
- **Evidence** (*measured*, `conformance-a/global-exit-handler.c`, 4 PEs, the
  VM's Slurm 24.11, three runs a row): `srun -K1` with a handler of 0 or 50 ms
  survives 3 of 3 with status 7; with 200 ms or 1,000 ms, 0 of 3 survive and
  the status is 143 (Slurm's own report: "tasks 1-3: Exited with exit code
  7", "task 0: Terminated", "srun-rc=143"). Without `-K1`, every handler
  length survives with status 7, the caller's handler starts 0.1-2.8 ms after
  the call, and the whole job is gone in about 10 ms on one node and 60-75 ms
  on two to four. Under `osoe-launch`, whose default also kills after a
  failure, the caller's output survived 2 of 3 runs with no handler at all.
  *Observed in source*: `shmem/tests/global-exit.sh` always passes `-k`, so no
  test runs a global exit under a launcher that kills on a bad exit, and none
  checks the launcher's own status.
- **Severity**: fix before release, because it is the README's own advice
  against the README's own promise, down to the job's exit status. Either hold
  the other PEs' `_exit` until the caller's process has ended (its node-mates
  can watch its pidfd; remote nodes can wait out a bounded part of the abort
  grace - design needed, since the caller may be a lead), or state the limit
  in the README beside `-K1`: a nonzero global exit leaves the caller about
  50 ms.
- **Confidence**: high on the behaviour; medium on the fix.

### C3. The README describes `OSOE_MAX_THREADS` wrongly, twice (fix before release; high; *measured*, mechanism lead-verified)

- **What it counts**: README "Limits" ("At most `OSOE_MAX_THREADS` threads of
  a PE in the library at once ... One more ends the job"), the reference row
  ("Application threads of a PE in the library at once"), `SHMEM_INFO`'s text
  (`init.c:95-97`) and the fatal itself (`metrics.c:537`, "more threads of this
  PE are in the library than ...") all say *at once*. The mechanism
  (`metrics.c:508-538`, lead-verified) claims a counter block on a thread's
  first counted call and releases it only in the thread's `pthread_key`
  destructor at thread exit (`:487-500`): the limit is on **live threads that
  have ever called the library**. A pool of 16 workers making strictly
  serialized calls, as an OpenMP team does, ends the job on the 16th worker's
  first call (*measured*, `conformance-b/threads-alive.c`: 15 workers exit 0;
  16, 17 and 20 end with the fatal; `OSOE_MAX_THREADS=21` with 20 workers
  exits 0). Spec Examples 14 and 16 (OpenMP, a context per thread) hit it at
  `OMP_NUM_THREADS` of 16. The design record has it right ("A PE's
  `OSOE_MAX_THREADS` blocks can all be taken by live threads", §4.11 and §10).
  The only test of the limit, `shmem.multiple.exhaust`, parks its workers
  inside waits, which satisfies both readings.
- **Where it is read**: only the node's first PE reads the variable
  (`metrics.c:460`, while the lead builds the control segment); every other
  PE copies the lead's value from the segment header (`node.c:888`) and never
  consults its own environment. The README says "the same on every PE of a
  node"; a user who sets it for one rank with the launcher's
  `-e RANK:NAME=VALUE`, which the README documents, gets silence and no
  effect, where the sibling `OSOE_MAX_CONTEXTS` is compared PE by PE and fails
  loudly (`node.c:840-847`).
- **Severity**: fix before release: wording in the Limits, the table,
  `SHMEM_INFO` and the fatal ("live threads that have called the library; a
  slot is freed when the thread exits; the node's first PE's value is the
  node's"), and a thought about the default of 16 on many-core nodes (scale
  deliverable, defaults table). Releasing a block when a thread leaves its
  outermost call is a design change: the block also indexes the thread's wait
  record.

### C4. Eight departures or choices the README's Limits section does not list (fix before release, all documentation; high; each *measured* or *observed in source*)

The section says it lists "every limit a program can meet, marked where it
departs from the specification or takes a choice the specification leaves
open". Missing:

1. **A split whose parent is per node spends one team number per node**
   (`team.c:813-840`: every parent's rank 0 claims from world PE 0's job-wide
   counter; *measured* by both `conformance-b`, `shared-split.c`, and `scaling`,
   `split_shared.c`, independently). Past `OSOE_MAX_TEAMS` - 3 nodes the same
   line of code succeeds on some nodes and returns nonzero on others, and a
   program that ignores the status runs its per-node collectives as no-ops on
   the losing nodes (a collective on `SHMEM_TEAM_INVALID` returns 1 and does
   nothing: `collectives.c:111-113`, allowed by §9.10.7's Notes). Scale
   finding P1 has the arithmetic; the README's only worked example,
   `split_2d`'s 2√N, is the best case.
2. **Collectives mismatched within a node are silent** (design line
   4587-4592 says so; *measured*, `conformance-b/mismatch.c`: a non-lead PE
   that broadcasts while the others reduce, or passes a different count, root
   or fcollect size, gets or gives wrong data and the job exits 0, on
   `0,0,0,0` and `0,0,1,1` alike). Mismatches between node leads are named
   ("collective epochs have diverged"), and one PE skipping a collective is
   caught, by the detector at 11 s on one node. Undefined behaviour, so not a
   defect; the "Loud, never hung" section should say where loud stops.
3. **`shmem_team_create_ctx` ignores the team's `num_contexts`**
   (`context.c:163-216`; §9.4.3: "By default, no contexts can be created on a
   new team"; osoe's own tests require creation to succeed on teams split
   with `config_mask` 0). Permissive, harmless to conforming programs, and a
   choice the README says it lists.
4. **`SHMEM_SYMMETRIC_SIZE=0` ends the job** (`bootstrap.c:168-170`; §8 allows
   a non-negative value, and a program using only static symmetric variables
   would run). Deliberate and tested (`CMakeLists.txt:2094`); unlisted.
5. **A process that exits without `shmem_finalize` ends the whole job with
   status 1** (`init.c`, `exit_without_finalize`, the loud answer to the
   second review's D14). Older programs that return from `main` without
   finalizing succeed elsewhere. Unlisted in "How a job ends".
6. **`shmem_init_thread` never returns nonzero**; every initialization failure
   ends the program (`init.c`; §9.2.1 says nonzero on failure). Defensible in a
   multi-process job; unlisted.
7. **A wait woken only by a plain store through `shmem_ptr` wakes at the
   100 ms sweep** (`region-wait.c`, `base/deadline.h:16`; *measured*,
   `conformance-a/waits.c`: a node-mate ping-pong by store through the pointer
   averaged 115,771 µs a round trip against 28 µs by `shmem_atomic_set`,
   `shmem_p` or `putmem_signal`, every round trip over 50 ms). Conforming -
   §9.11.1 requires wait variables to be updated by AMOs or puts - and a
   thousandfold cost of a common intranode idiom when porting from
   implementations that poll the word. One line under Limits, naming
   `shmem_atomic_set`.
8. **The cache routines were removed from the specification in 1.5**
   (Annex F: last version supported 1.4; Annex G.2 item 1, lead-verified), so
   "the deprecated cache routines do nothing, as their deprecation allows" is
   inexact: osoe carries them as no-ops for old programs, and §5's "shmem.h
   ... must contain only the interfaces ... defined in this specification"
   strictly forbids declaring them. Harmless; say "removed in 1.5; kept as
   no-ops".

### C5. Two undefined inputs corrupt memory silently where the project's policy is to end the job by name (after release; high; *measured*)

- **`iput`/`iget` strides of zero or less** (`rma-typed.c:88, 104` check
  nothing; §9.6.1.3 and §9.6.1.7 require a stride of at least 1). *Measured*
  (`conformance-a/argcheck.c`): `shmem_long_iput(next, src, -1, 1, 8, pe)` wrote
  four elements into the tail of the *previous* allocation and returned;
  stride 0 landed only the last element. The block-strided forms end the job
  by name for the analogous input (`:131-165`). Two compares.
- **A collective allocation with different sizes on different PEs
  desynchronizes the heaps silently** (`malloc.c`, `allocate()` takes the
  barrier but compares nothing; Annex C). *Measured* (`malloc-mismatch.c`):
  after `shmem_malloc(64)` on PE 0 and `shmem_malloc(4096)` on PE 1, the next
  allocation sat at offset 64 on one and 4096 on the other, a later `shmem_p`
  landed inside the wrong object, and the job exited 0. The one common misuse
  in these families osoe does not make loud; a debug-mode compare of (size,
  alignment, hints) folded into the allocation barrier would.

### C6. Minor: stale comments and one unlocked read

- `deprecated.c:182` and design §7.4's table say `pstart_pes` does not exist;
  `deprecated.c:60-107` defines it (the author's decision of 2026-09-26).
  `team.c:1105` and `test-team.c:339` cite "§9.2.3" for `shmem_team_ptr`
  (§9.1.10). `context.c:8-10` says the level stays at most SERIALIZED;
  `init.c:60-62` describes the interim mutex slice 3 removed;
  `test-active-set.c:3-4` calls the whole job the only active set.
- `context.c:263-275` (`osoe_context_team_destroyed`) and `:96-107` (`resolve`)
  read the context pool without `pool_mutex` while `create` and `destroy`
  write it under the mutex: under `MULTIPLE`, a team destroyed on one thread
  while another manages contexts on a different team is a data race
  ThreadSanitizer would report; no wrong outcome was found and none is
  tested (low confidence; *observed in source*).

## The routine families, one line each

| Family (§) | Verdict | Strongest evidence |
|---|---|---|
| 9.1 setup, exit, query | conforms, except C2, C4.5, C4.6 | `lifecycle.c`: three sessions with nested `init_thread` at other levels; a non-last finalize a barrier; `query_initialized` 0, 1, 1, 0; `global_exit` statuses reaching `srun` (7, 0, 3, 11 with four callers) without `-K1` |
| 9.1.7-9.1.10 accessibility, `shmem_ptr`, `shmem_team_ptr` | conforms | `ptr-access.c` on four maps and under `OSOE_FORCE_REMOTE`: non-NULL exactly for node-mates, heap and statics; `SHMEM_TEAM_SHARED` exactly the set `shmem_ptr` reaches, `{self}` under force-remote; §7's contract for every member |
| 9.1.11-12 info | conforms | `_Static_assert` on the vendor string; the version 1.6 |
| 9.2 threads | conforms, C3 aside | `init_thread` grants what is asked up to MULTIPLE, `shmem_init` gives SERIALIZED (listed); callable before init; `provided` never a refusal |
| 9.3 memory | conforms, C5 aside | `memory.c`: zero sizes NULL; exhaustion NULL on every PE with heaps in step; `realloc` moves, fails intact, keeps hints; `calloc` zero-fills recycled memory; `align` 8 B to 512 KiB; the seven no-action calls take no barrier |
| 9.4 teams | conforms | `split2d.c` (6 configurations, 1-4 nodes, 0 failures): negative and zero strides, invalid triplets the same status on every PE, all-or-nothing `split_2d`, `xrange` not dividing N; translate both ways; destroy tears down shareable contexts, ends the job on a live private one |
| 9.5 contexts | conforms, C4.3 aside | pool exhaustion nonzero and `SHMEM_CTX_INVALID`; NOSTORE recorded and fences kept; `ctx_get_team`; a PE outside a team's context ends the job by name |
| 9.6 RMA | conforms, C5 aside | 1,398 names exported and declared; type-exact C11 dispatch for all of Table 5 with and without a context; zero lengths with NULL pointers |
| 9.7 AMO | conforms | `amo-atomicity.c`: 4 PEs × 4,000 of fetch-add, fetch-inc, CAS loops, fetch-or and `put_signal ADD` on one word on `0,0,0,0`, `0,0,1,1`, `0,1,2,3`: every total exact, every fetched value distinct; one `osoe_rmw_apply` serves both paths |
| 9.8 signals | conforms, except C1 | `ordering.c`: 60 rounds × 64 KiB across nodes with 1 KiB chunks and split wires: the data always there when the signal was |
| 9.9 sessions | conforms | accepted and inert (§9.9 permits); `SHMEM_CTX_INVALID`, nested start, double stop no-ops |
| 9.10 collectives, 9.10.10 scans | conforms | reduction and scan type lists equal Tables 10 and 11; the 1.6 broadcast writes the root's `dest`; `alltoalls` indexing as Example 33; `collect` with per-PE counts including 0; Example 37 and an in-place `float _Complex` inscan on four maps; two threads' collectives on two teams, 200 rounds, three maps, release and debug with perturbation, 0 failures |
| Annex F collectives | conforms | `broadcast32/64` leaves the root's `dest` alone on the whole-job path too; subsets use member-only trees; `pSync` restored to `SHMEM_SYNC_VALUE` and checked word by word; callers outside their set named |
| 9.11 point-to-point | conforms, C4.7 aside | `waits.c`: `_any` fairness, `_some` finds all, empty and all-masked sets 1, `SIZE_MAX` and 0; signed and unsigned comparisons; waits on statics woken across nodes; the deprecated `short` waits |
| 9.12 ordering | conforms | `ordering.c` on three maps with small chunks and split wires: put, fence, flag always ordered at the target; a third PE reads complete memory after `quiet` and `pe_quiet({2})`; 64 gets of 64 KiB past the landing zone complete at quiet |
| 9.13 locks | conforms | `lock-order.c`: FCFS at 150 ms spacing; `test_lock` 0 and 1; `clear_lock`'s quiet; 1,200 contended increments exact across nodes |
| Annex F other | conforms | `start_pes` twice, implicit finalize (launcher 0, `/dev/shm` clean), the old names and waits, `_SHMEM_*` constants |
| §8 environment | conforms, C4.4 aside | the size grammar including the spec's own examples; invalid values named; `SMA_` honoured and `SHMEM_` wins |
| Headers | conforms, except C1 | C99, C11 and C17 with `-pedantic-errors`; C++11, 17, 20 with g++ and clang++ `-pedantic -Werror`; handles as static initializers; a C++ program linked with `OSOE_CC=g++ oshcc` has symmetric statics; interposing `shmem_long_p` and `shmem_quiet` works, generic calls included |
| Argument checks | loud where it matters | bad PE, non-symmetric address, range past the heap, unaligned AMO or lock, reentrant `set_lock`, `clear_lock` of an unheld lock, bad comparison or signal operator, `SHMEM_CTX_INVALID` in RMA, `free` of an interior pointer, `align(2)` and `(3)` all end the job naming the routine; silent only in C5 |

## The SOS suite as evidence, and what no test exercises

The runbook's sweep (159 of its programs on two maps; the four that do not
pass explained there and confirmed by `conformance-a`: the two generic
extensions to `short`, the buffer overflow osoe stops by name, and
`shmem_align(2)`, which osoe refuses by design) is real evidence for the
routines it calls, and it calls them one node map at a time, mostly with
defaults. Requirements that **no test exercises**, in either suite, the
pacer's registered tests included:

1. The C11 four-argument `shmem_signal_add`/`shmem_signal_set` (C1).
2. The launcher-reported status of `shmem_global_exit`, and the caller's
   handlers under a launcher that kills on a bad exit (C2); `global-exit.sh`
   uses `-k` only.
3. `SHMEM_VERSION`, `SHMEM_INFO`, `SHMEM_DEBUG` and their `SMA_` spellings.
4. Fence ordering and quiet completion **across nodes as seen by the target
   or a third PE**: `test-quiet.c` runs on one node only, where puts are
   `memcpy`s; `test-internode.c` checks counters, "not that it ordered" (its
   own comment). The investigator's `ordering.c` shows the property holds; the
   suite should own it.
5. Collectives run from several threads at once on different teams; a split
   or destroy from a non-main thread beside another thread's collectives;
   `shmem_global_exit` from a worker thread (all *measured* to work by
   `conformance-b`; `threads-teams.c` could join the suite as is).
6. A serialized thread pool against `OSOE_MAX_THREADS` (C3); `split_2d` with
   an incomplete last row (only the hand-run SOS sweep); mismatched
   collectives within a node (C4.2); `iput`/`iget` strides of zero or less and
   mismatched allocations (C5); a wait woken by a `shmem_ptr` store (C4.7).

## What was read, and what was not

Read end to end against the code: §9.1 to §9.13, §8, §7 (`SHMEM_TEAM_SHARED`),
Annex C (return values of memory routines, zero-length transfers), Annex F
(the deprecated API, the active-set collectives), Annex G.2 (the change list
for the cache routines and the wait-variable rule), the C11 generics annex
(every synopsis compiled). §3 (symmetric objects, atomicity guarantees) and §4
(progress, invoking, thread safety) were read for the rules the routines
depend on, not clause by clause. Not read this round: Fortran (none exists;
the README says so), the profiling interface's text (the mechanism was
checked by the `install` investigator), and the session routines' semantics
"if they are ever more than no-ops" (§10), which stay no-ops.

## What is sound

- **Surface**: every routine name of 1.6's C interface, exported weak over a
  strong `pshmem_`, declared in both headers, compiling under C99 to C17 and
  C++11 to C++20; every C11 generic but C1's.
- **Atomicity across paths**: node-mates' CPU atomics and the agent's are one
  instruction on one mapping (`rmw.h`), and 16,000-operation mixed-issuer
  contention runs stayed exact with unique fetched values.
- **Completion and ordering**: fence, quiet, `pe_quiet`, put-with-signal and
  nonblocking gets held across nodes with 1 KiB chunks and randomly split
  wires; every wait's predicate is a `seq_cst` load, which is what makes a
  signal imply its data on aarch64.
- **Teams and contexts**: every argument rule, the all-or-nothing split, the
  translation both ways, `SHMEM_TEAM_SHARED` as exactly the set `shmem_ptr`
  reaches, destroy's teardown of shareable contexts.
- **Collectives**: type tables equal to the specification's, the 1.6 root
  rule and Annex F's root rule both right, `alltoalls` indexing, `collect`
  with unequal counts, in-place reductions and scans, `pSync` restored, and
  cross-team collectives from several threads.
- **Lifecycle**: nested and repeated sessions, a non-last finalize as a
  barrier, `start_pes` with implicit finalization, status propagation without
  `-K1`.
- **Loud where undefined**, with the two exceptions of C5 and the within-node
  collective mismatch of C4.2.
- **README Limits entries in these families that are accurate**: `shmem_ptr`
  NULL off-node; sessions inert; statics symmetric only under `oshcc`, C++
  included; `shmem_init` giving SERIALIZED; C only; `OSOE_MAX_CONTEXTS` and
  `OSOE_MAX_TEAMS` as stated in units of teams and contexts.

## Sources

`reports/conformance-a.md` and `conformance-a/*.c` (fifteen probes),
`reports/conformance-b.md` and `conformance-b/src/*.c`, `spec-1.6.txt`
(`pdftotext -layout` of the 1.6 PDF), `README.md` Limits and Reference,
design §3.15, §4.6, §4.9, §4.11, §5.4, §7.4, §10, `docs/pi-cluster.md` §8 (the
SOS sweep), `shmem/src/metrics.c:487-538` and `abort.c` (lead-verified).
