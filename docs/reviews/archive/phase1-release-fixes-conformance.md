# Round four, question 1: is osoe OpenSHMEM 1.6? (after the release fixes)

Review of `main` at `08b3da0`, run 2026-10-03 against
`docs/reviews/adversarial-review-prompt.md`, with the changes its lead gave:
the tree is the one the release fixes left (`docs/decisions/2026-10-02-release-fixes.md`,
Parts 1 to 6), the previous round is `phase1-release-conformance.md` (`b0e0b53`), and
the team redesign (`docs/design/team-resources-proposal.md`, step D's brief
`docs/prompts/phase1-step7-slice3-prompt.md`) is decided but not built. Labels:
*documented*, *observed in source*, *measured*, *recalled*. Section numbers are the
specification's (`docs/reference/OpenSHMEM-1.6.pdf`, present, read as
`pdftotext -layout`: `build/scratch/review-r3/spec-1.6.txt`).

**How it was run.** The tree was copied into `osoe-load` (`/tmp/conformance-r4/src`,
identical file for file to the review worktree, checked by SHA-1 over all 1,370
files) and built from scratch with the `release` preset: 26 s, no warnings. Programs
were built with that build's `oshcc` and run under its `osoe-launch` on one and
several logical nodes, under `/tmp/osoe-vm.lock`. **`osoe-launch` without `-k`
stands in for `srun -K1`**: it terminates the survivors at the first nonzero exit,
as `KillOnBadExit` does - but it reacts within a millisecond where Slurm took about
100 ms in the plan's measurement (`phase1-release-fixes-plan.md` §1 point 1), and it
does not count its own kills in its status, where `srun` counts a task it killed as
143 (the fixes record, Part 2 item 2). `osoe-load` has no Slurm, and the Pi cluster
was not used; the one run that needs it is described under F1. The probes are under
`/tmp/conformance-r4/probes/` in `osoe-load` - the previous round's, rebuilt, in
`prev-a` and `prev-b`, this round's in `new/` - with this round's also in this
session's scratchpad; the SOS sweep's summary is `/tmp/conformance-r4/sos/sweep.txt`
and the examples' `/tmp/conformance-r4/examples.txt`. The tree's copy, the build and
every binary were removed at the end (sources and summaries kept, 420 KiB).

## Verdict

**Yes - every defined behaviour checked is implemented as the text says, the
previous round's two gaps are closed, and its README findings are now true of
the tree.** The C11 four-argument signal forms compile and dispatch with
team-relative PE numbers (C1); a single caller of `shmem_global_exit` keeps its
status, handlers and output under a launcher that kills on a bad exit (C2); the
README describes `OSOE_MAX_THREADS` exactly (C3) and now lists all eight
behaviours it lacked (C4), each entry checked true. Sixteen behaviours the
specification defines for invalid handles and zero arguments, the §8 size grammar
with its own examples, the 1.6 block-strided transfers (which no SOS program
calls), a quiet completing another thread's puts as a third node sees it,
`split_2d`'s rules for a large `xrange`, and the team and context limits as the
README states them all hold, and the previous round's twenty-three probes behave on
this tree as they did on `b0e0b53`. The SOS suite gives 159 programs passing on two
maps here, as the README says of the VM - and 161 of 165 with the two programs
its sweep script omits, as it says of the Pi - with the same four exceptions.

Two things remain, both in `shmem_global_exit`, both cheap to state and neither
cheap to change:

- **F1**: with callers on more than one node - the ordinary shape of an error path
  every PE detects - a launcher that kills on a bad exit (`srun -K1`, which the
  README recommends) kills PEs of the later node, callers among them, in most runs
  with a fast launcher, and the window it has (110 ms and more in 13 runs of 20,
  415 ms and more with 300 ms handlers) exceeds Slurm's reaction of about 100 ms;
  under `srun` that is status 143 against §9.1.6's one "shall", and lost output
  and handlers. The README states the residual, but as a coincidence ("at the same
  moment") costing only the status. **Fix the sentence before release**; a single
  job-wide carrier after; a Pi run would settle the rate under Slurm.
- **F2**: the caller's `atexit` handlers and C++ static destructors run on the
  library's agent thread, not the caller's, so a handler sees another thread's
  thread-local storage and the caller's C++ `thread_local` objects are never
  destroyed. Not new in the fixes (it was so at `b0e0b53`), found by no round.
  A README sentence; the mechanism after release.

Nothing found blocks release. From this question's side, once the teams are
built, osoe can be released after F1's sentence (and F2's, which is one more
line). §10's still-blocking "a thorough 1.6 read" can be closed for the C
interface: with the previous round's reading of §9.1 to §9.13, §8, Annexes C and F
and the generics, this round read §10 (the profiling interface, which the
previous round did not), §3.2, §4, §5 and the text of every family it probed again.

## The previous round's findings, checked as claims

- **C1, the C11 four-argument `shmem_signal_add`/`shmem_signal_set`: fixed, and
  holds** (*measured*). The previous round's failing probe
  (`generic-signal-ctx.c`) now compiles with gcc and clang at `-std=c11` and
  `c17`, `-pedantic-errors -Werror`; so does its probe of every other C11
  generic. At run time (`signal-c11.c`, six maps of 4 and 6 PEs on one to four
  nodes, 0 failures): the four-argument forms reach `shmem_ctx_signal_*` with the
  PE taken **relative to the context's team**, as §9.8.5's and §9.8.7's argument
  tables require (a team of the odd world PEs, each signalling its team neighbour);
  the three-argument forms, their addresses as function pointers, the C99
  spelling, and a profiler written with the name in parentheses (one call
  intercepted, the four-argument call left to `shmem_ctx_signal_add`) all
  work. As the README's "C only" bullet states, a profiler written as §10's
  Example 48 is, defining `shmem_signal_add` by its plain name after including
  `pshmem.h`, does not compile in C11 (*measured*): see M1.
- **C2, `shmem_global_exit` under a launcher that kills on a bad exit: fixed for
  one caller, and holds** (*measured*). On `0,0,1,1`, `0,1,2,3` and `0,0,0,0`,
  callers PE 0, 1 and 3 with a 200 ms handler, without `-k`: the launcher
  returned the caller's status every time, the handler finished, every PE's
  buffered line was written, nobody was killed. A 3 s handler finished; a 12 s
  one was cut at 10.2 s with the status kept, as "How a job ends" says. The
  specification's Example 8 as written returns 1; the examples' `global_exit.c`,
  which calls straight after `shmem_init`, returned its 99 in 24 runs of 24 on
  four nodes of one PE, four of two and eight of one, nobody killed; a caller on
  a worker thread under `MULTIPLE`, its main thread in a wait that never ends,
  returns its status with its handler run; a program that registered
  `atexit(shmem_finalize)` returns its status and runs its later handlers;
  `start_pes` programs with the caller first or last return 3. **Several
  callers on more than one node is where it does not hold**: F1.
- **C3, `OSOE_MAX_THREADS`: fixed, and holds** (*measured*). With
  `OSOE_MAX_THREADS=2` and five live workers each making one serialized call
  (`places.c`): the sixteen query routines (`my_pe`, `n_pes`, `team_my_pe`,
  `team_n_pes`, `team_translate_pe`, `pe_accessible`, `addr_accessible`, `ptr`,
  `team_ptr`, `query_thread`, `query_initialized`, `info_get_version`,
  `info_get_name`, `ctx_get_team`, `team_get_config`, `pcontrol`) take no place;
  `p`, `g`, `fence`, `quiet`, `test` and `atomic_fetch` to the PE itself each
  take one, and the job ends naming the variable ("a thread keeps its place
  until it exits"). Exactly the README's Limits entry and settings row;
  `SHMEM_INFO`'s text says the same. Fifteen serialized workers pass, sixteen
  end the job, and twenty pass with `OSOE_MAX_THREADS=21`, as before.
- **C4, the eight unlisted behaviours: all eight are now in the README, and
  each entry checked is true** (*measured* where marked, *observed in source*
  otherwise): the per-node team cost (Teams, marked temporary); collectives
  mismatched within a node (Failures; and "Loud, never hung" now says where loud
  stops) - *measured* again with the previous round's `mismatch.c`: a different
  kind, count, root or `fcollect` size on a non-lead PE exits 0 on `0,0,0,0` and
  `0,0,1,1` and is named on `0,1,0,1`; `num_contexts` not enforced (`context.c`
  `create` never reads it); `SHMEM_SYMMETRIC_SIZE=0` refused (*measured*: every
  PE ends naming the variable); exit without finalize (Failures);
  `shmem_init_thread` returning 0 or not at all (`init.c:536-556`); the
  `shmem_ptr` store found at the 100 ms sweep (*measured*: the previous round's
  `waits.c` still takes 38 s for its rounds); the cache routines "deprecated in
  1.3 and left the specification after 1.4" (*documented*: Annex F's table, last
  version supported 1.4). Their declarations stay in `shmem.h`: M5.
- **C5, silent corruption on undefined input: unchanged, as rated (after
  release)** (*measured*, the previous round's `argcheck.c`): `iput` with a
  destination stride of 0 lands only the last element; with -1 it writes four
  elements into the tail of the previous allocation, and the job exits 0, where
  `ibput`'s stride below `bsize` ends the job naming §9.6.1.4. §9.6.1.3 does
  require "greater than or equal to 1", so this stays undefined input made
  silent, not a departure. Mismatched `shmem_malloc` sizes still desynchronize
  the heaps silently (`malloc-mismatch.c`: offsets 64 and 4,096, a later put
  landing in the wrong object). The record (Part 4) left both out of the README
  because "no draft carried its evidence", a reason about the process, not the
  behaviour; the rating stands.
- **C6, stale comments and the unlocked pool read: unchanged** (*observed in
  source*). `context.c` `resolve` and `osoe_context_team_destroyed` still read
  `pool[i].live` without `pool_mutex` while `create` and `destroy` write it under
  it; `team.c`'s header (step 3) still says a split borrows a word from the
  symmetric heap, which `split` no longer does (`team.c:975-978`; *measured*: a
  heap of `1m` allocates 1,048,576 bytes whole after a split and a `collect`).
  Minor, as before.

## New findings

### F1. Several callers of `shmem_global_exit` on more than one node: a killing launcher kills PEs, callers among them, in most runs, and the README states this too narrowly (fix the sentence before release; the mechanism after release; confidence high on the measurement, medium on the rate under Slurm)

- **Location**: `shmem/src/abort.c:115-177` and `abort.h:12-38` (each node's
  first caller carries that node's end); `progress-host.c:606-620` with
  `progress.c:1351-1361`, `osoe_progress_peers_ended`, which skips a node whose
  goodbye named another caller ("each waiting for the other would only run both
  to the bound"); README "How a job ends", second paragraph ("Two limits remain
  ... two PEs of two nodes that call `shmem_global_exit` at the same moment ...
  `srun` may then return 143"); spec §9.1.6.
- **Scenario**: a program decides on every PE that it cannot go on - a check
  every PE makes alike, a convergence test after a reduction, an input file none
  can open - and every PE calls `shmem_global_exit(EXIT_FAILURE)`: the
  specification's Example 8 run on every PE rather than on PE 0. On a job of two
  or more nodes each node's first caller carries its node's end, so there are
  several carriers, and none waits for a node whose goodbye named another
  caller. The first to exit nonzero makes a launcher that kills on a bad exit
  terminate the other nodes' PEs that have not yet ended - their carriers among
  them, in their handlers or before their flush.
- **Evidence** (*measured*). `new/ge-all.c`: every PE prints a buffered line,
  registers a 0 ms `atexit` handler and calls `shmem_global_exit(EXIT_FAILURE)`
  after a barrier; ten runs a map, `osoe-launch` without `-k`. A PE was killed by
  the launcher in **8 of 10** runs on `0,0,1,1`, **7 of 10** on `0,1,2,3`, **7 of
  10** on two nodes of four PEs, and 0 of 10 on one node; each killed PE lost its
  buffered line and its handler (for example "lines=2 handlers=1 killed=2" of
  four). With `-k`, where the launcher kills nothing, the same program returned 1
  in 20 runs of 20 with every line written: the library's own end is right, and
  the loss is the launcher's kill. With two callers of different statuses on two
  nodes (`new/ge-callers.c`, PEs 0 and 2 of `0,0,1,1`, 300 ms handlers), 4 runs of
  8 killed a PE of the other node, once the other caller itself, which never ran
  its handler. `osoe-launch` returned a status passed every time only because it
  does not count its own kills; `srun -K1` counts a task it killed as 143, which
  `srun` then returns (the fixes record, Part 2 item 2; the plan's table, §3.1).
  `abort_exit_model.py`'s NO_KILL already fails on two callers on two nodes (the
  record, Part 2 item 4): the residual is known; its reach was not.
- **Why it matters**: §9.1.6's one "shall" is this case: "If more than one PE
  calls `shmem_global_exit`, then the exit status returned to the environment
  shall be one of the values passed". The routine also "flushes I/O ... in
  accordance with C/C++ language requirements for normal program termination",
  which a caller killed by the launcher does not do. The README recommends `-K1`
  and states the residual as two calls "at the same moment" costing the status;
  what was measured is the ordinary shape of a program's error path, in most runs
  with a fast launcher, costing callers' output and handlers too. The previous
  round's C2 was rated fix before release as "the README's own advice against the
  README's own promise"; this is the same for the case the record left stated.
- **The window Slurm would have** (*measured*, each rank wrapped to stamp its
  end, `-k` so that nothing is killed): from the first nonzero exit to the last
  PE's end, `ge-all` with instant handlers took **110 to 230 ms in 13 runs of
  20** (0 or 1 ms in the rest) on `0,0,1,1` and `0,1,2,3`; with every PE calling
  and 300 ms handlers on `0,1,2,3`, where each PE is its node's carrier, **415 to
  850 ms in 6 of 6**. Slurm took about 100 ms to kill in the plan's measurement,
  so `srun -K1` should kill in a good share of the first kind of run and nearly
  every run of the second. The tail of about 110 ms appears only with callers on
  two nodes - a single caller's job ends within a few milliseconds of its
  handler - and looks like a wait that wakes on the 100 ms sweep (the leads' end
  loops wait on the ring with `OSOE_SWEEP_NS` as their timeout,
  `progress-host.c:602-604` and `:617-619`; *not verified*). Making that wait
  wake on its event would shrink the window below Slurm's reaction for programs
  with quick handlers, a partial cure cheaper than the one below.
- **What would settle the rate under Slurm** (for the lead; *not run*): on the Pi
  cluster, `srun -K1 --cpu-bind=none -N2 --ntasks-per-node=2 ./ge-all` twenty
  times, then `ge-callers 0,1,2,3 7 300` at `-N4 --ntasks-per-node=1`; count
  statuses of 143 against the values passed, and the `LINE-BEFORE` and
  `HANDLER-DONE` lines. A rate near zero in both would lower this to the
  sentence alone.
- **Fix**: before release, the README sentence, with M2's: under `-K1`, callers
  on more than one node - which a program whose every PE detects the same failure
  makes - can have Slurm kill PEs of the later node, callers included, so `srun`
  may return 143 and those PEs' buffered output and handlers are lost; write
  diagnostics to standard error, or call from one PE. After release, one
  job-wide carrier: a node that learns of a lower-ranked caller on another node
  (its goodbye already names the caller, `peer_abort_origin`) leaves with 0 and
  stops waiting for that node, and the lowest-ranked caller's node waits for
  every other node's end of stream - then nothing exits nonzero before the last
  PE elsewhere has ended. It changes the model-checked abort protocol, so the
  model comes first and the call is the author's.

### F2. The caller's handlers run on the library's agent thread, not the caller's (after release, with a README sentence now; confidence high on the behaviour, medium that it departs from the specification)

- **Location**: `shmem/src/abort.c:94-96` (`if (osoe_agent_speaks()) for (;;)
  pause();`: any thread but the agent waits for good) and `:170-172` (the agent
  calls `exit(carrier ? status : 0)`); `agent.c:211-215`, `:261-265`;
  `init.c:517-526`. Unchanged since `b0e0b53` (*observed in source*: the same
  test of `osoe_agent_speaks` and the same `pause` there).
- **Scenario**: a C program keeps its last error in a `_Thread_local` buffer and
  registers an `atexit` handler that prints it; a C++ program's logger keeps
  per-thread records in a `thread_local` object whose destructor flushes them, as
  tracing and logging libraries do. Either calls `shmem_global_exit(7)` from the
  thread that holds the record.
- **Evidence** (*measured*, `new/ge-tls.c` and `new/ge-tls.cpp`, two PEs on two
  nodes): with plain `exit(7)` after `shmem_finalize`, the handler runs "on the
  caller's thread: yes" and prints the message, and the C++ destructor prints
  "the last record". With `shmem_global_exit(7)`, the handler runs "on the
  caller's thread: NO" and prints an empty message, and the C++ destructor never
  runs; the status is 7 either way.
- **Why it matters**: §9.1.6 has the routine flush I/O and release resources "in
  accordance with C/C++ language requirements for normal program termination".
  C++'s `std::exit` destroys the calling thread's `thread_local` objects first;
  here the calling thread is the agent, so the program's are left. The Notes let
  "I/O done by other means (e.g., third party I/O libraries)" be implementation
  dependent, so this is defensible, but the README says "the calling PE leaves
  with `exit(status)`, so its `atexit` handlers run ... as the specification
  asks", and a reader takes that to mean as if the caller had called `exit`.
- **Fix**: now, a sentence in "How a job ends": the handlers run on the library's
  thread, not the caller's - a handler reading the caller's thread-local storage
  sees another thread's, and a C++ caller's `thread_local` objects are not
  destroyed. After release, if wanted: the calling thread waits until its agent
  has told the network and then calls `exit` itself, the agent standing aside -
  the abort protocol's "exactly one thread ends the process" kept, with the other
  thread chosen; `abort_exit_model.py` first.

### Minor

- **M1.** The plain-name profiler of §10's Example 48 cannot define
  `shmem_signal_add`, `shmem_signal_set` or `shmem_sync` in C11, because
  `pshmem.h` includes `shmem.h`, whose macros then expand the definition
  (*measured*: `signal-wrap-plain.c`, "expected declaration specifiers"). The
  README says so and gives the parenthesized spelling, which works. Inherent
  while the two C11 forms share a name with the C/C++ form; a `pshmem.h` that
  included `shmem.h` without its C11 macros would let Example 48's pattern work
  for all three.
- **M2.** A caller whose call loses the race to another PE's notification ends as
  a non-caller, without its handlers (`abort.c:39-43` admits it: "a caller whose
  declaration loses may go unseen"). It is not rare when every PE calls
  (*measured*, `ge-all.c` with `-k`): at least one calling PE's handler was
  skipped in 6 of 10 runs on `0,0,1,1` and 7 of 10 on `0,1,2,3`, where every PE is
  its node's only caller and the race is with the other nodes' goodbyes; with two
  callers on one node, in 2 of 11. §9.1.6's notification reaches such a PE as its
  own call does, so this conforms, but the README's "the other callers leave with
  0 after their handlers" is false in most of those runs. One word in F1's
  sentence ("usually").
- **M3.** A fractional `SHMEM_SYMMETRIC_SIZE` gives a heap of exactly the ceiling
  §8 asks for - `3.1M` is 3,250,586 bytes, the specification's own example - but
  the allocator's 16-byte blocks then refuse `shmem_malloc(3250586)` (NULL), as
  `1.0000001k` refuses 1,025 (*measured*). A heap rounded up to the block would
  make "at least as large" usable whole.
- **M4.** `docs/pi-cluster.md` §8's "What to expect" is stale (*measured*): of the
  examples, `bar_pair` and `bcast4` are not refused (active sets smaller than the
  job have been supported since step 5 slice 7), `global_exit` exits 99 under
  `osoe-launch`, not 1 (Part 2 made the launcher's status `srun`'s), and
  `addr-accessible` exits 0 - 68 of 72 exit 0 on `0,0,1,1`, the other four the
  expected `nullput`, `just-a-shmalloc`, `shmem_2dheat` (1) and `global_exit`
  (99); and SOS's `shmem_team_b2b_collectives` is stopped by name on every map of
  more than one PE, not on `0,0,1,1` alone - its `dest` holds 10 longs and it
  collects 10 a PE. Differences for the better, which the runbook's rule ("a
  program that passed there and fails here is the finding") would misread. And
  `build/scratch/sos/sweep.sh`, the VM's sweep, leaves out SOS's two automake
  variants, which is why the README gives 159 for the VM and 161 for the Pi.
- **M5.** The six cache routines, removed after 1.4 (Annex F), are declared in
  `shmem.h` (`shmem.h:939-946`), where §5 allows "only the interfaces and
  constant names defined in this specification", and `shmemx.h`, where §5 puts
  everything else, is empty. Harmless (the previous round's C4.8); the README now
  gives their history rightly and does not say where they are declared.

### Refuted, or no finding

- **A cross-thread quiet that seemed to deliver misplaced data** (*measured*,
  then refuted). A first version of `mt-quiet3.c` - thread A puts, thread B
  quiets and flags a third PE on a third node, which gets the data - failed in
  about 4 runs of 9 on `0,1,2,0,1,2` with split wires, reading values such as
  `0x14023D` for index 11389. Decoded, each is a torn word: round 13's value for
  that index with its low byte or two from round 14's. My probe let thread A
  start the next round's puts while the observer's get was still reading, and
  §3.2 guarantees exclusivity only among atomic operations, so a get racing a
  put may see a torn word. With that race removed (`mt-quiet4.c`), the variant
  with the quiet passed 8 runs of 8 and the variant without it failed with stale
  data (round 44's value in round 45): the probe can fail, and a quiet by one
  thread completes another thread's puts on the shared default context (§9.2
  item 2 with §9.12.2). A first probe of the same property, with data and flag
  to one PE, could not fail - one ordered connection carries both - and is not
  counted.
- **Partially overlapping reduction and scan buffers** (§9.10.9, §9.10.10:
  undefined): nothing checks them, but the result is as if the source were read
  before the destination was written (`overlap.c`, `sum_reduce` and
  `sum_inscan` with `dest = source + 1`, on one node and four). Undefined input
  with a benign outcome; no finding.
- **Allocation hints** (§9.3.5: advisory, "If the implementation cannot
  optimize, the behavior is same as `shmem_malloc`"): `malloc.c` calls them "the
  only thing that says which bytes may carry remote atomics", but on this
  backend `osoe_transport_set_range` is a counter and nothing else
  (`transport-am.c:1963-1971`), so unhinted memory behaves alike. A note for
  the seam: a backend must not need the hints to be correct.

## The routine families

What this round measured; the previous round's table stands for the rest, its
probes having been rerun.

- **Invalid handles and zero arguments** - every behaviour the text defines
  (*measured*, `invalid.c`, 0 failures on `0,0,1,1`, `0,1,2,3`, `0,0,0,0`,
  `0,0,1,1,2,2`): `shmem_team_ptr(INVALID)` NULL and `(WORLD)` equal to
  `shmem_ptr` (§9.1.10); `team_my_pe`, `team_n_pes` -1 (§9.4.1-2);
  `team_get_config(INVALID)` nonzero and writing nothing, and `(WORLD, 0, NULL)`
  0 (§9.4.4); `translate_pe` -1 for either team invalid and for PEs -1 and N
  (§9.4.5); both splits of an `INVALID` parent nonzero with `INVALID` handles
  (§9.4.6-7); `team_destroy(INVALID)` and `ctx_destroy(INVALID)` no-ops;
  `team_create_ctx(INVALID)` nonzero with `SHMEM_CTX_INVALID` (§9.5.2);
  `ctx_get_team` `INVALID` and nonzero, `WORLD` for the default and a created
  context (§9.5.4); session start and stop, `ctx_fence`, `ctx_quiet` on
  `SHMEM_CTX_INVALID` no-ops (§9.9.2, §9.12.1-2); `pe_quiet` with `npes` 0; a
  split's `num_contexts` returned by `get_config`; `ctx_pe_quiet` on a team
  context with a team-relative target. Sessions accept any options, as §9.9.2's
  "no combination of options ... results in undefined behavior" requires
  (*observed*, `session.c`).
- **The query routines answer rather than end the job** (*observed*,
  `accessible.c`): `pe_accessible` 0 for a PE outside the job, `addr_accessible`
  0 for an address that is not symmetric (a static in a program not linked by
  `oshcc` among them).
- **Signals' atomicity with the new forms (§9.8.1)** (*measured*,
  `signal-atomic.c`): every PE adds to PE 0's signal word 3,000 times each by
  `shmem_signal_add` with three arguments, by the C11 four-argument form on a
  context of the world reversed, and by `shmem_putmem_signal` with
  `SHMEM_SIGNAL_ADD`; PE 0's `shmem_signal_wait_until` for the exact total
  (36,000 on four PEs, 54,000 on six) returned on `0,0,0,0`, `0,0,1,1`,
  `0,1,2,3` and `0,1,2,0,1,2`, with every put's data in place. A lost add would
  have left it waiting until the launcher's timeout.
- **Block-strided RMA (§9.6.1.4, §9.6.1.8, new in 1.6, called by no SOS
  program)** (*measured*, `ibstride.c`): `uint64_ibput`, `ctx_ibput64`, the C11
  `shmem_ibget`, `ibput128` and `ibget8` over nine shapes (strides equal to and
  above `bsize`, `nblocks` 0 and 1, a source stride of 1,000) place every element
  where the definition puts it, on one node, two, four, and with 1 KiB chunks
  and split wires. Swapping the two strides in one call makes the probe fail.
- **Teams** (*measured*): the limit as the README states it (`teams-exhaust.c`):
  61 splits of the world, each destroyed at once, succeed; the 62nd returns
  nonzero on every PE with `SHMEM_TEAM_INVALID`, naming `OSOE_MAX_TEAMS`; after
  `shmem_finalize` and a new `shmem_init` another 61 succeed; with
  `OSOE_MAX_TEAMS=4`, one. `split_2d` with `xrange` 6, 10 and 100 over six PEs
  behaves as with 6, as §9.4.7 says, and with 3 over seven (an incomplete row),
  on the world, a reversed team and `SHMEM_TEAM_SHARED` (the previous round's
  `split2d.c`).
- **Contexts and the thread level** (*measured*, `limits.c`): `shmem_init`
  gives `SHMEM_THREAD_SERIALIZED`; 63 contexts beside the default at the default
  `OSOE_MAX_CONTEXTS`, 4,095 at 4,096, the next nonzero with
  `SHMEM_CTX_INVALID`, a destroyed one's place reused, a team context refused
  alike when the pool is full.
- **`SHMEM_THREAD_MULTIPLE`** (*measured*): a quiet by one thread completes
  another thread's puts on the shared default context, as a third PE on a third
  node sees it (`mt-quiet4.c`, above); collectives on two teams from two threads
  at once, and splits beside collectives (the previous round's
  `threads-teams.c`); a global exit from a worker thread.
- **Collectives' argument checks** (*observed in source*, with the previous
  round's measurements): a root outside the team is named at both ends
  (`collectives.c:114-116`, and `:242-243` for a negative one); `alltoalls`
  strides below 1 are named (`:503-505`, §9.10.6's "must be greater than or
  equal to 1"); a destination too small for `collect` is named (*measured*, SOS's
  `shmem_team_b2b_collectives`); a collective on `SHMEM_TEAM_INVALID` returns
  nonzero and does nothing, which §9.10.7's Notes leave to the implementation.
- **§8 environment** (*measured*, `heapfull.c` with `SHMEM_DEBUG`'s heap line):
  `20kk` 20,480 (characters after the multiplier ignored, as §8 says), `3.1M`
  3,250,586, `.5m` and `0.5m` 524,288, `1e6`, `2.5k`; `20 m`, `20x`, `-1m`,
  `0x10`, the empty string and `5p` refused by name; `SMA_SYMMETRIC_SIZE`
  honoured, `SHMEM_` winning over it; `SHMEM_VERSION`, `SHMEM_INFO`,
  `SHMEM_DEBUG` and their `SMA_` spellings each print, with any value including
  empty and `0` (§8: "Any"). A heap of `1m` allocates 1,048,576 bytes whole
  before and after a split and a `collect`: nothing the library does takes room
  from the program's heap (but M3).
- **The profiling interface (§10, not read by the previous round)** (*measured*
  and *observed*): no library code calls a public `shmem_` name - neither the
  source nor the disassembly of `libosoe.so` shows a call through one - so a
  profiler that replaces one routine never catches the library's own calls, and
  nothing is layered but the C11 macros, which `pshmem.h`'s comment documents
  (requirement 3); `shmem_pcontrol` exists (5); `pshmem.h` brings every type and
  constant (6); interposition works (the previous round's `profile-probe.c`,
  rerun). M1 is the one rough edge.
- **The surface** (*measured*): 1,787 weak `shmem_` symbols over 1,794 strong
  `pshmem_` ones, as at the previous round (the seven extra are `pshmem_`
  spellings of `start_pes`, `_my_pe`, `_num_pes` and the four old allocation
  names, beside their `pstart_pes`-style forms); the declarations of the first
  half's 1,398 routines link under C99, C11 and C17 with gcc and clang; every
  C11 generic compiles under `-pedantic-errors -Werror`; C++11, 17 and 20 with
  g++ and clang++.
- **The previous round's probes, rebuilt and rerun on this tree, behave as they
  did** (*measured*; twenty-three of them). Passing: `lifecycle` (three sessions,
  nested `init_thread` at other levels) on three maps; `ptr-access` on three maps
  and under `OSOE_FORCE_REMOTE`; `memory`; `nobarrier`; `waits` on two maps;
  `ordering` with 1 KiB chunks and split wires on two maps; `amo-atomicity`
  (put-with-signal ADD among the AMOs) on three; `lock-order` on two;
  `sessions-deprecated` on two; `profile-probe`; Example 37 and an in-place
  complex inscan on two; `shared-ptr` on three (`SHMEM_TEAM_SHARED` exactly the
  set `shmem_ptr` reaches); `split2d`; `threads-teams` in both modes;
  `thread-exit`; `threads-alive`; and the four compile and link probes
  (`decl-check`, `generic-ok`, `generic-signal-ctx`, `cxx-probe`). Showing the
  documented behaviour, unchanged: `argcheck` and `malloc-mismatch` (C5) and
  `mismatch` (C4's silence within a node).
- **The OpenSHMEM examples** (*measured*): 68 of 72 exit 0 on `0,0,1,1`, the
  other four as expected (M4).

## The SOS suite as evidence, and what no test exercises

**The sweep, on this tree** (*measured*, `build/scratch/sos/sweep.sh` adapted to
build in `/tmp`, each program on `0,1` and `0,0,1,1`): 163 programs (the 166
sources less the two MPI programs and a helper), 161 built, **159 passed on both
maps** - the README's VM figure, measured on the tree of 28 September. The script
does not build the two programs SOS's automake makes from other sources with
extra flags, `rma_coverage_pshmem` (`-DTEST_PSHMEM`, the profiling names) and
`query_thread_funneled` (`-DENABLE_THREADS`); built by hand, both pass on both
maps, so the suite whole is **165 programs, 163 built, 161 passing on both maps:
exactly the Pi's figure** (`docs/pi-cluster-results.md:574`), and the README's two
counts differ only by those two. The four that do not pass are the four
documented, for the documented reasons: two pass `short *` to the
`_all`/`_any`/`_some` generics, which 1.5 and 1.6 define over the AMO types only;
`shmem_team_b2b_collectives` collects past its own `dest`, named;
`shmemalign` asks for an alignment of 2, refused by name.

**What SOS cannot tell us.** The suite predates 1.6: *no SOS program* calls
`shmem_pe_quiet` or `shmem_ctx_pe_quiet`, `shmem_signal_add` or
`shmem_signal_set`, `ibput` or `ibget`, `sum_inscan` or `sum_exscan`,
`shmem_ctx_session_start` or `stop`, `broadcastmem`, `collectmem`,
`fcollectmem`, `alltoallmem` or `alltoallsmem`, `shmem_query_initialized`,
`SHMEM_CTX_NOSTORE`, `start_pes`, `_my_pe` or `_num_pes`, or the cache routines
(*measured*, by `grep` over `unit/`, `spec-example/` and `apps/`). Its 161 are
evidence for the 1.5 interface. Everything new in 1.6 rests on osoe's own suite
(`test-quiet.c`, `test-signal.c`, `test-typed-rma.c`, `test-scan.c`,
`test-surface.c`, `test-broadcast.c`, `test-collect.c`, `test-alltoall.c`,
`test-context.c`, `test-start-pes.c` and others; each family has one), the
stress tester, and this round's and the previous round's probes - the tests
written by the process that wrote the code, which is why the probes above check
1.6's block strides and signal forms against the text independently.

**Requirements no test in any suite exercises**, after the fixes (the previous
round's list, revisited):

1. Several callers of `shmem_global_exit` on more than one node under a launcher
   that kills (F1); the model shows NO_KILL failing, and no test runs it.
2. Which thread runs the caller's handlers (F2).
3. Collectives from several threads at once on different teams, and a split
   beside another thread's collectives: still in no suite (the previous round's
   item 5); measured working again here.
4. Fence ordering and quiet completion across nodes as a third PE sees them: no
   suite test was added (the previous round's item 4); the probes hold the
   property (`ordering.c` rerun; `mt-quiet4.c` for a quiet across threads).
5. `SHMEM_DEBUG`, and `SMA_VERSION`, `SMA_INFO` and `SMA_DEBUG`: the fixes added
   tests of `SHMEM_VERSION` and `SHMEM_INFO`, and one of `SMA_SYMMETRIC_SIZE`;
   these four remain untested (measured working).
6. `iput`/`iget` strides of zero or less, mismatched allocations (C5); a
   fractional heap size filled whole (M3).

The previous round's other items are now tested: the four-argument signal forms
(`test-signal.c`, `shmem.generic-arity.*`, the header tests), the global exit's
status under a launcher that kills, with handlers (`global-exit.sh`'s `kills.*`
modes), and a serialized thread pool against `OSOE_MAX_THREADS`
(`shmem.thread-places.*`).

## The team redesign, read for conformance

Read: the proposal's §§0-5 and step D's brief whole, against §9.4 (preamble,
§9.4.3, §9.4.6-8) and §9.10's preamble. **Nothing found that either step would
get wrong against the text.**

- **D2's mask reduction** gives every parent PE the same reduced word, so a split
  that cannot be made fails "on all PEs in the parent team" alike (§9.4.6), and
  `split_2d` takes all its numbers from one result, keeping §9.4.7's
  all-or-nothing rule. The split's closing sync is kept, so the parent and the
  new teams stay "immediately usable ... without any intervening synchronization"
  (§9.4 preamble).
- **D6's handles** stay opaque values that compare equal to
  `SHMEM_TEAM_INVALID` only when invalid, and a destroyed handle - undefined to
  use, §9.4.8 - is named rather than meeting the next team under its number.
- **D7**'s mask token makes splits of *different* parents running on threads of
  one PE at once take turns, by a retry that never fails a split and always lets
  the lowest parent number through; §9.4 already forbids simultaneous splits of
  the *same* parent, so a conforming program loses time at most, never a team.
- **E's lane per thread** keeps §9.4's "multiple collective operations on
  different teams may be performed in parallel" free of cross-team waits (E9
  gives the reason: a shared pool could close a cycle across PEs). **E1's class
  rule** - every lead must classify a collective's bytes alike - relies on what
  the specification already requires of conforming programs (the same `nelems`
  on every PE of a reduction, broadcast or scan), and leaves `collect`, whose
  sizes differ, on the unchanged gather (E7); a program that breaks it is outside
  the specification and is named.
- **For the README after D**: the `num_contexts` entry (permissive) is untouched
  by either step and stays true; the brief already rewrites the three temporary
  team entries and the `OSOE_MAX_TEAMS` row.

## What is sound, and what convinced me

- **The fixes to this question's findings hold as claims**: C1 by compiling the
  previous round's failing probe and running the four-argument forms on a team
  context; C2 by 24 runs of the examples' `global_exit` and the single-caller
  matrix; C3 by sixteen query routines and six operations against a limit of two;
  C4 by checking each new README entry against the code or a run.
- **No defined behaviour found implemented wrongly** in what this round re-read
  and probed: invalid handles (sixteen behaviours), the §8 grammar to its own
  examples, block-strided transfers, signals with team-relative PEs, the team and
  context limits, `split_2d`'s `xrange` rules, cross-thread quiet as a third node
  sees it, the profiling interface's requirements.
- **The previous round's evidence reproduces on this tree**: its twenty-three
  probes behave as they did, the SOS suite gives the same 159 (161 of 165 with
  its two automake variants, the Pi's figure) and the same four exceptions, and
  the examples do better than the runbook expects.
- **The global exit's own end is right** when no launcher interferes: with `-k`,
  every multi-caller run returned a status passed with every buffered line
  written (20 of 20), and a single caller is right under a killing launcher too.
- **Loud where undefined** in the collectives' arguments (roots, strides, short
  destinations), as before; silent still in C5's two cases, as rated.

## Sources

The specification (`spec-1.6.txt`): §3.2, §4, §5, §7, §8, §9.1 (all), §9.2, §9.3.5,
§9.4 preamble and §9.4.3-8, §9.5.3-4, §9.6.1.3-4, §9.8.1-3, §9.8.5, §9.8.7, §9.9.2,
§9.10 preamble, §9.10.5-7, §9.10.10, §9.12.1-3, §10, Annex F's table and F.2.7. The
tree at `08b3da0`:
`README.md`; `shmem/src/abort.c`, `abort.h`, `init.c`, `agent.c`, `progress.c`,
`progress-host.c`, `context.c`, `team.c`, `collectives.c`, `malloc.c`,
`accessible.c`, `session.c`, `rma-typed.c`, `quiet.c`, `transport-am.c`,
`base/size.c`, `base/order.h`; `shmem/include/shmem.h`, `pshmem.h`, `shmemx.h`;
`docs/pi-cluster.md` §8; `docs/pi-cluster-results.md`; the fixes plan, decisions and
record; the team proposal and step D's brief. The previous round's probes,
`build/scratch/review-r3/conformance-a/` and `conformance-b/src/`. This round's
probes and logs: `/tmp/conformance-r4/probes/new/` in `osoe-load` (`signal-c11.c`,
`signal-c99.c`, `signal-wrap-*.c`, `ge-callers.c`, `ge-all.c`, `ge-example8.c`,
`ge-finalize-handler.c`, `ge-startpes.c`, `ge-thread.c`, `ge-tls.c`, `ge-tls.cpp`,
`places.c`, `invalid.c`, `heapfull.c`, `ibstride.c`, `teams-exhaust.c`, `limits.c`,
`mt-quiet*.c`, `overlap.c`, `signal-atomic.c`; `../ts.sh` stamps each rank's end),
the SOS sweep in `/tmp/conformance-r4/sos/` (`sos-sweep.sh`, `sweep.txt`), and the
corpus's `tests-sos` and `openshmem-examples`. The probe sources, the scripts and
the sweep's results are mirrored on the host in the session scratchpad,
`<scratchpad>/probes/`, a scratch directory outside the repository
(`new/`, `sos/`, and the previous round's under `prev-a/` and `prev-b/`). The
build in `osoe-load` was removed when the review finished; a refutation run
rebuilds the tree (26 s) and the probes with its `oshcc`.
