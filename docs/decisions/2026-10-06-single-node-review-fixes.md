# 2026-10-06: the fixes of v0.2 step 1's merge-point review

The arc's first full adversarial review ran on `main` at `7053819`, the tree
v0.2 step 1 left (`docs/decisions/2026-10-06-single-node-seam.md`,
`2026-10-06-single-node-build.md`), as the step's brief and the author's prompt
for the night ask: "run it and fix what it finds before step 2". Five
investigators, the refutation of every finding rated *fix before release*, and a
synthesis, `docs/reviews/archive/v0.2-step1-review.md`, whose verdict is that
the step passes its merge point after the named changes - in the procedure's
terms, release after named changes; nothing blocks. This record is the changes,
made on the branch `step1-review-fixes` from `main` at `53ec6cf`, by Claude
under the night's mandate; the review's questions for the author are in the
synthesis and in the night's record.

## The finding rated fix before release

1. **`osoe-stat --summary` on a job of one node** (readiness Q5-1): it printed,
   for each team, the tree's epochs, which only the lead's engine keeps and a
   job of one node never runs - "epoch 0/0" while three PEs waited in a barrier
   - and the README described it as each team's barrier in progress. A fresh
   refuter confirmed it - facts, scope and severity - in the source and on
   Rocky, narrowing two of its words (the synthesis). Fixed beyond the
   investigator's "before release" sentence: the summary now says, for a job of
   one node, "one node: no tree to walk; the PE lines (osoe-stat JOB) say where
   each PE waits"; the README's "Looking inside a job" and `--help` say the walk
   is between nodes; `shmem.stat` checks the line, and that "epoch 0/0" is gone
   (`2d1b33e`). *Undo*: the branch in `summary_line`.

## The correctness investigator's findings

None was rated *fix before release*; each is a test or a sentence, none a
defect of the code.

2. **One-node coverage lost beyond the record's list** (K1, after the
   release): tests `internode` only for the map they were written on, whose
   subject a job of one node has too. Registered on one node, in both builds:
   the global exit's remaining modes - a status of 0, two callers, the grace's
   two held-stdout cases and an end while PEs stream to a node-mate (the last
   three not under ThreadSanitizer, as across nodes) - and, under a launcher
   that ends the job at the first bad exit, a slow handler, a non-lead caller,
   two callers and a held stdout; the deadlock detector's re-init, mismatch,
   sleeping and off on `0,0,0,0`; the destroyed team's refusal on `0,0`; core
   exclusion on `0,0`, its test expecting a connection area only in a job of
   more than one node and a sibling heap for each other PE of the node; and the
   pacer's threads exercise under ThreadSanitizer on one PE, `0`, beside `0,1`.
   What needs its script changed for one node - `shmem.leftovers.*`, a lead
   dying in init, the pacer's plants - stays on the checklist, where step 1's
   record put it.
3. **The one-node quiet's fence has no test that fails without it** (K2, after
   the release): on aarch64 the counters around it, `ldar` and `stlr`, keep the
   order themselves; built `-march=armv8.3-a` the investigator's litmus finds
   the forbidden outcome without it, and on x86-64 it is all that orders a store
   before a later load. `transport-am.c` says so beside it; the litmus as a test
   is on the checklist.
4. **A wake lost on the one-node agent's word passed the suite** (K3, minor):
   `shmem.agent-wake.L4`, the fastest of five sessions' finalizes under 50 ms
   against the agent's 100 ms sweep - 0.1 ms on Rocky, and 100.3 ms with the
   agent's wait in the private scope, which fails it.
5. **The classic wait takes the thread's timer slack** (K4, minor): `futex.h`
   and `futex-classic.c` say so, and why no deadline of the library's minds.
6. **Liveness without `pidfd_open` had no test** (K5, minor): `deny-pidfd.c`, a
   seccomp filter that refuses the call with `ENOSYS` as a kernel before 5.3
   does, around `shmem.dead-pe.no-pidfd.{sibling,lead}.L4`, in both builds -
   the death found by pid and the lead's notice printed once. With the pid
   check blinded the death goes unseen and a survivor computes out its 30 s,
   and both fail; the pidfd cases beside them still pass.
7. **Two sentences of the step's record** (K6, minor): corrected in place.
8. **Unexplained**: one of the investigator's eight loaded runs left a failed
   start's tombstone that seven more did not. No test of the suite pipes the
   launcher into a reader that exits early, the one way found to leave one
   (the broken pipe, above).

## The other findings, each fixed unless it says otherwise

- **Readiness** (`2d1b33e`): Q5-2, the desktop section's example says that its
  eight PEs need about 2.1 GiB of `/dev/shm` and how to run them on less, names
  the ring and where to run from, lists `make`, and gives the suite's counts in
  words, the numbers kept in "How osoe is tested" (two fewer without a C++
  compiler); the models at `-j2` on 4 GiB. Q5-3, the four directory READMEs say
  what the step changed. Q5-4, the `/dev/shm` refusal names `OSOE_MAX_THREADS`
  among the settings that size the region. Q5-5, `shmem.single-node.refuse-nodes`
  fails if any PE reached its control segment, and
  `shmem.single-node.version-says-so` holds the suffix as a literal. Q5-7, the
  checklist strikes the done item and lists what the step left to a later one;
  Part 1's choice 2 goes to the author with this review. Q5-9, `osoe-launch
  --help` says what each option does, and the timeout's line names `-t`. Q5-10,
  the release evidence's `job.sh` has its licence line. Q5-11, the probe removes
  its object's name before it prints. **Q5-6**, the full build of `main` calling
  itself 0.1.0, is the author's to choose (below). **Q5-8**, the seam (after the
  release): `internode-backend.h` now says that an RDMA backend replaces
  `transport-am.c` as well - `osoe_transport_set_range` lives there, and design
  §4.10 dispatches node-mates per PE and operation class where the first part
  dispatches per PE (`69a81b2`); §4.10's note and the runtime a backend may call
  are on the checklist's after-release list.
- **Conformance** (`a4445b0`, `f122357`): C1, a heap whose size is not a
  multiple of 16 bytes held the multiple below, so §8's own example, `3.1M`,
  could not hold its 3,250,586 bytes: the size is rounded up, inside the page
  the internal region starts at, and `shmem.heap-size.{3.1M,1000}` allocate the
  whole value on every PE. C2, the README's SOS sentence names the tagged tree's
  sweep and the single-node build's. C3, `SHMEM_INFO` in the single-node build
  says which of its settings do nothing there, from a string CMake sets as it
  sets the version. C4, §9.1.6's details on one node run in both builds
  (`shmem.global-exit.*.one-node`), and `SHMEM_DEBUG`, `SMA_VERSION`,
  `SMA_INFO` and `SMA_DEBUG` have tests.
- **Scale and performance**: Q3-1 (after the release), a test that nothing in
  the library spins, is on the checklist, best before step 4, whose helpers
  rewrite the progress threads' waits; a wait that spins 200 µs before it
  sleeps passes both suites today. Q3-2, the step's record says its 8- and
  32-PE runs used `SHMEM_SYMMETRIC_SIZE=16m` (`9ced0eb`). The investigator's
  note for readiness, confirmed by the lead: a refused start piped into a reader
  that exits early left its control segment in `/dev/shm`, the launcher killed
  by SIGPIPE before it removed leftovers. The launcher now ignores SIGPIPE,
  giving each child SIGPIPE as it found it, and a PE's fatal path holds SIGPIPE
  back as the abort path does, so that it tells its node and leaves with the
  fatal's status; `shmem.dev-shm-capacity.broken-pipe`, in both builds, starts
  a refused start into a pipe whose reader is gone, and fails without either
  change - the segment left and 141, or 141 (`e725ad8`, `24dda52`). Its note for
  portability: Rocky 9.8's 5.14 kernel has the futex2 calls, backported, so the
  README names io_uring off by default and no liburing headers as what keeps
  the full build from it (`dfe7cfc`).
- **Portability** (`dfe7cfc`): Q1, `shmem.strict-c11` failed on GCC 16 for two
  test programs' spin counters, set and never read: each is read once. Q2,
  `shmem.strict-c11` and `shmem.static-link` built their nested trees with the
  default `cc`: they take the configured compiler, its target and toolchain
  file (not its flags, which carry the presets' sanitizers). Q3, the README's
  x86-64 passages say which tree ran, and that the single-node build runs under
  Rosetta with no shim, 343 of its 349 tests. Q4, Ubuntu 22.04 with pip's CMake,
  and the error its own CMake gives. Q5, without pkg-config the full build's
  configure reaches the message that names `-DOSOE_SINGLE_NODE=ON`. Q6 and Q7
  were readiness Q5-2 and Q5-3. Q8, a 64-bit system in both requirement lists.
  Q9, the pidfd notice before Linux 5.3 in the README; the Rosetta stall's
  workaround on the checklist, for the next x86-64 run; the futex2 refusal's
  wording under an emulator stays the after-release list's item; Rocky's
  RPATH, as the investigator judged, is probably what a user wants.

## For the author

- **The version of `main` between tags** (readiness Q5-6): it calls itself
  0.1.0, the tag's version, so a report's `SHMEM_VERSION=1` line cannot tell a
  build of `main` from the release. Recommendation: 0.1.90 from the merge after
  a tag until the next one - a number that sorts between the two releases,
  which CMake's `project(VERSION)` takes as it is and the soname's major, 0,
  does not see - with the README's Status saying so. *Not done*: the README's
  "calls itself 0.1.0 until v0.2 is released" stands until the author chooses.
- **Part 1's choice 2**, the io_uring start-up check moved below the seam into
  the transport's settings check (`2026-10-06-single-node-seam.md`), still
  awaits the author's look, as the step's record says.

## Verification

On the tree that merges, `1a486d2` (the code; this record's commit adds only
documents), each run under its VM's lock:

| | debug | release | AddressSanitizer | ThreadSanitizer |
|---|---|---|---|---|
| full build, Debian 13 (`osoe`), the pins on | 760 of 760 | 759 of 760, then `shmem.floor` 3 of 3 | 757 of 757 | 526 of 526 |
| single-node build, Debian 13, liburing hidden | 381 of 381 | 381 of 381 | 378 of 378 | 373 of 373 |
| single-node build, Rocky Linux 9.8 | 379 of 379 | 379 of 379 | 376 of 376 | 371 of 371 |

Beside them: `tier3` 113 of 113; the pacer's collectives at three values of
`OSOE_SMALL_COLLECTIVE_BYTES`, its teams and threads, and ten minutes of `mix`,
each `failures=0`; the single-node build installed on Debian and on Rocky, `oshcc`
from the prefix, three programs at 8 and 32 PEs, and a job of two logical nodes
refused by name with nothing left in `/dev/shm`. The one failure in the release
suite was `shmem.floor`'s stream pin, 0.094 of the floor against a pin of 0.10,
while three VMs ran suites on the host at once - its fastest trips 2.45 times
the floor's against the 1.9 to 2.0 of the morning, the library's code changed
since `57e58d1`, whose run passed the pin, in comments alone; run alone after
the suites, it passed three times of three. Rocky's counts are two fewer
than Debian's: its `gcc make cmake` installs no C++ compiler, which the
headers' C++ test needs. The models are unchanged since `7053819`, whose run
had them whole, and were not run again.

The same runs on `57e58d1`, before the correctness findings' fixes, all
passed: the full build 742, 742, 739 and 513, `tier3` 113, every pacer run
with no failure; the single-node build 363, 363, 360 and 360 on Debian and
361, 361, 358 and 358 on Rocky.

Each new test was first shown to fail without what it tests: the broken pipe
without the launcher's change (the segment left, status 141) and without the
fatal path's (status 141); the agent's wake with its wait in the private
scope (100.3 ms against 0.1); the pid fallback blinded (a survivor computing
out its 30 s). The nested-build checks were run with Clang as the configured
compiler, their caches naming it, and the whole tree built warning-free with
Clang under `-std=c11` in both builds, once `test-reply.c`'s tickets started at
zero.
