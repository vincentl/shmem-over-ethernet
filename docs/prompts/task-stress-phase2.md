# Task: the stress tester, phase 2 - the newest code

**Status: written on 2026-09-27, at the end of phase 1, from
`docs/design/osoe-pacer-proposal.md` §5 and what phase 1 built and found
(`docs/decisions/2026-09-27.md`).** Phase 1 left `osoe-pacer` running: the
driver, `parallel_print`, `sort`, `alltoall`, `print`, a plant for every
check, and the smoke tests. Phase 2 points it at the code most likely to hold
what the tests have not found: the lock-free shared-context path of
`SHMEM_THREAD_MULTIPLE` (step 8 slice 3), the signal and wait routines, and
teams made and used many at once.

The threaded exercise is here, second rather than last, because of one open
item: `shmem.multiple.checked.0-1-2-0-1-0` fired `OSOE_CHECK_QUIET` once in a
debug suite, on 2026-09-26, and the message was not kept
(`docs/decisions/2026-09-26-evening.md` item 13; unreproduced in three suites
and some 1,300 runs since). An exercise that runs for an hour under
`OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1` is the best chance of seeing it again,
with its message.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work here (design §1).
- **For the tool itself: nothing that grows faster than O(N) a PE**, N the
  job's size, and an oracle - with a plant that fires it - for everything it
  measures.

## Read first

1. `CLAUDE.md`, the naming rule especially.
2. The proposal, `docs/design/osoe-pacer-proposal.md`: §3, §4.3, §4.4,
   §4.7, §5's phase 2.
3. `docs/decisions/2026-09-27.md`: phase 1 as built - the interface and why
   it differs from the brief (`memory` fits the exercise itself; `step` takes
   `last`; `checkpoint` may reduce and print), the oracle review, the plants
   and how their tests assert every line.
4. `tools/pacer/exercise.h`, `osoe-pacer.c`, and one exercise whole -
   `sort.c` - before writing another; `parallel-print.h` for its rule about
   calls on two teams in turn.
5. `shmem/tests/test-multiple.c` (the threaded shape and the two hooks) and
   `docs/decisions/2026-09-26-evening.md` items 10 to 13 (shared contexts,
   the hooks, the open firing).
6. The spec, `docs/reference/OpenSHMEM-1.6.pdf`: §9.2 (`init_thread`,
   `query_thread`), §9.4 (`team_split_2d`, `team_create_ctx`,
   `team_get_config`, `team_ptr`, `team_destroy`), §9.5 (contexts), §9.9
   (sessions), §9.8 (`putmem_signal_nbi`, `signal_fetch`,
   `signal_wait_until`), §9.10.10 (the scans), §9.11 (every `wait_until` and
   `test` form, the `_vector` forms).
7. Prior art: SOS's `mt_a2a`, `mt_contention`, `mt_membar`, `many-ctx`,
   `shmem_team_context`; PRK `Synch_p2p` (trace U4) for signal pipelines.

## What to build

1. **`threads`** (proposal §4.7), named only, since it initializes with
   `SHMEM_THREAD_MULTIPLE` as `osoe-bench threads` does. `--threads T`,
   default 4, refused above `OSOE_MAX_THREADS` as the environment gives it.
   Each thread has a private context and a serialized one, and shares the
   default context and a team context (`shmem_team_create_ctx` on a split of
   the world); each runs slices of fetch-and-add accounting and of the
   mailbox on each context in turn, with `shmem_ctx_fence`,
   `shmem_ctx_quiet` and `shmem_ctx_pe_quiet`, and
   `shmem_ctx_session_start` and `_stop` around bursts. Oracles as the parent
   work's, summed over the threads. The driver stays on the main thread:
   `step` releases the threads for `units` slices and waits for them;
   `checkpoint` and `parallel_print` run with the threads parked. Every
   thread's failures go through `pacer_failure`, which must then take a lock.
2. **`mailbox`** (§4.4): N slots a PE, a payload and a signal word each;
   `putmem_signal` and its nonblocking form with `SHMEM_SIGNAL_SET` and
   `SHMEM_SIGNAL_ADD`; receivers taking turns at every waiting and testing
   form of §9.11, `signal_wait_until` and `signal_fetch`; each message
   checked whole on wake (a trailer equal to its head proves the payload
   landed before the signal), the slot reset by its receiver; at the end the
   sent and received counts compared pairwise. Wake latency within a node;
   a ping-pong's round trip across nodes, since clocks differ there.
3. **`teams`** (§4.3): a budget of teams made once (16 of the default's 61
   creatable) of every shape - strided with random start, stride and size,
   negative strides, a team of one, a split of a split, `split_2d` rows and
   columns - every query checked against arithmetic; then rounds of
   collectives alternating between each PE's row and column teams, out of
   phase, over a few types and operators, the scans among them; every input
   (PE, round)-determined. `--teams-exhaust`, a short mode apart, splits
   until the split returns nonzero, checks it did on every PE and that the
   job goes on. Team numbers are never reused (design §5.7): nothing in the
   exercise may destroy and re-split in a loop.
4. **Plants**, one for every new check, asserted line by line through
   `cmake/PacerPlant.cmake` - the rule the author set on 2026-09-27.
5. **The hooks.** The debug and asan smoke runs of `threads` set
   `OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1`, which matter once shared
   contexts exist (proposal §3, "Tests", point 3).

Not in this phase: `atomics` whole, `rma`, `locks`, `memory`, the
collectives sweep, the interposition shim, `mix`, the library's counters in
the stream, the second route of `parallel_print`, the deprecated forms,
installation.

## Order of building, each part running before the next

1. `mailbox`, on the four maps; its plants.
2. `teams`, then `--teams-exhaust`; its plants.
3. `threads`, first on private contexts only, then the shared and team
   contexts, then sessions; its plants; the hooks in debug and asan.
4. The tests, the runbook's §6, the measurements, the notes.

## Tests

As phase 1's, in the same block of `tools/pacer/CMakeLists.txt`, whose place
in every preset is the author's to decide (phase 1's record gives what it
costs): each new exercise `--quick` over the four maps, one map under
ThreadSanitizer - where `threads` belongs, since tsan is what sees a race
between threads - and a plant test a check.

## The measurement

An hour of `threads` in a debug build with both hooks on, on the VM and on
the Pi at 4 × 4; ten minutes each of `mailbox` and `teams` at 4 × 4 in
release. If the `OSOE_CHECK_QUIET` firing comes back, its message, the seed,
the map and the command line are the finding: record them and stop.

## Stop and ask

- The tool finds a failure in the library - the open firing above all.
  Record a reproduction; whether to fix it on the branch is the author's call.
- A check no plant can fire, or a plant caught by a check it was not meant for.
- Anything in the tool that grows faster than O(N) a PE.
- The team budget: if the exercise makes the cost of never reusing a team
  number vivid, show the author before shaping around it.
- The quick runs cannot be kept to about a minute a preset in release.
