# Cloud session 4: the response

*The account of the fourth cloud session (`docs/prompts/cloud-session-4-prompt.md`),
2026-10-14, branch `cloud-session-4` from `main` at `7cd2d68`. The per-item log,
with every quoted line and command, is `docs/prompts/cloud-session-4-manifest.md`;
this file is the whole account, written when the session's credit ran low - so it
is shorter than planned, and says plainly what was not run.*

## In short

Four of the seven batches are done, one commit each, all pushed:

| Batch | Commit | Items done | Stopped |
|---|---|---|---|
| 1, the settings' diagnostics | `4f7253c` | D16, D24, D27, D29 | - |
| 2, start-up's messages | `ef98ffb` | D10, D11, D14, D15, D25, D28 | - |
| 3, `fatal.c`'s and `abort.c`'s | `f122cc2` | D18, D19, D20, D21, D22 | - |
| 4, `osoe-stat`'s | `4f030c8` | D2, D3, D5, D6, D7 | D8 (a layout change) |

Twenty items done, one stopped. Every done item has a test that fails without it,
shown by setting the change aside and running the batch's tests (manifest, "How
fails without was shown"). Batches 5, 6 and 7 (D23, D30; the P items; the T items)
were not started. No wire or layout change was made.

**Not run, and the lead's merge should run**: the release suite on batch 4's
tree (batches 1-3 each ran it whole and clean); the debug, asan and tsan presets
for any batch; the single-node build's suite. See "What the lead's merge needs".

## The environment

As the third session's: Linux 6.18.44-fc-v114, four vCPUs, 15.7 GiB, `/dev/shm`
16 GiB, `kernel.io_uring_disabled = 0`, liburing 2.9, gcc 13.3 as `cc` (gcc 14.2 and
clang 18.1 beside it), cmake 3.28.3, Python 3.13.16; no netem. At `main`: release
built in 63 s, 0 warnings; static checks 13 of 13 in 98 s. From batch 1 the static
checks' expression gains `settings-known`:
`ctest --test-dir build/release -j4 -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|license-lines|single-node\.configure|version\.)'`.

## Every item

Each item's quoted evidence, files and reasoning are in the manifest under its id;
here, the outcome and what a reviewer needs.

### Batch 1 (`4f7253c`)

- **D16 - done.** `shmem/src/unknown-settings.c`, `.h` (new, runtime layer): the
  103 names osoe reads; rank 0 says once a process, after `SHMEM_INFO`, each set
  `OSOE_`/`SHMEM_` name it does not read, "... is not a variable osoe reads, and
  changes nothing: did you mean X?" (edit distance 2 at most) or "(SHMEM_INFO=1
  lists the ones it reads)". A notice, not a refusal (a decision: a site's
  wrapper may set names of another version). `cmake/CheckSettingsKnown.cmake` and
  `shmem.settings-known` keep the list equal to the names the library reads.
  Tests `shmem.settings.unknown.{near,far,known}`. Wire/layout: neither.
- **D24 - done.** `context.c`: a full context pool says `OSOE_MAX_CONTEXTS` once a
  PE. Test `shmem.context.pool-3.said-once.L2`.
- **D27 - done.** `base/ring-check.c` `osoe_ring_sysctl_refuses()`;
  `transport-internode.c`'s `EPERM` hint names `kernel.io_uring_disabled` only where
  it refuses, with its value and, at 1, the group. Test
  `shmem.refuse.no-io-uring-setup` (updated). The sysctl's branches shown by hand
  at 2, at 1 as user 65534, and at 1 as root (manifest); no test can set it.
- **D29 - done.** `init.c`'s `SHMEM_INFO` names every variable of the README's
  tables (new sections for `OSOE_COLLECTIVE_PLACES`, the transport's buffers, the
  testing variables). Test `shmem.version.info.every-variable`
  (`shmem/tests/info-every-variable.sh`).
- Runs: batch tests without 9 of 15, with 15 of 15; release suite 936 tests, 933
  passed, 2 skipped, 1 failed (`shmem.boundary`: the new file in no layer list,
  fixed in `cmake/CheckBoundary.cmake`); static checks then 15 of 15.

### Batch 2 (`ef98ffb`)

- **D10 - done.** `progress.h`'s host gains `describe_node`; `progress-host.c`
  says a node by its host and its lead's rank; `progress.c`'s seven death messages
  use it. Wire/layout: neither (the host table is process-local).
- **D11 - done.** `node.c`, `liveness.c`: start-up messages give the rank beside
  the local id. Left as they were: the "the local lead has N" agreement messages
  (a decision; they compare values, not PEs).
- **D14 - done.** `listener.c`: `EMFILE`/`ENFILE` say the descriptors a mesh needs
  and `ulimit -Hn`.
- **D15 - done.** `base/stall-watch.h` `osoe_stall_watch_limit_beyond(limit,
  hops)` with a 2 s grace a hop; the listener's five waits and `job.c`'s follow wait
  the limit plus their distance, so the node with the missing rank speaks first.
  Design note §3.15 says so.
- **D25 - done.** `transport-internode.c`: an issue record of which node each
  landing extent is owed by (`static _Atomic uint32_t *issue_target`, release on
  store, acquire on load, §3.16 named); a stalled get names that node.
- **D28 - done.** `base/futex.h` `osoe_futex_kernel_has_calls(release)`: a refusal
  of futex2 on a kernel that has it is named a filter (seccomp, a sandbox), not an
  old kernel; `init.c` wording; `refusal.sh`.
- Runs: batch tests without 19 of 30, with 30 of 30; static 15 of 15; release
  suite 940 tests, 938 passed, 2 skipped, none failed.

### Batch 3 (`f122cc2`)

- **D18, D20, D21 - done.** `fatal.h`, `fatal.c` `osoe_fatal_first_word()`
  (seq_cst exchange, once a process); `abort.c`, `liveness.c`, `init.c` take it
  before writing; the liveness line now ends "; this PE was ..." since it silences
  the "ending" line that said so. Tests
  `shmem.dead-pe.lead.0-0-1-1-2-2.helpers-{0,2}.one-line` (`dead-pe.sh` ONE_LINE).
- **D19 - done.** `blocked.h`/`metrics.c` `osoe_blocked_finalizing()`,
  `osoe_blocked_where()`: "in shmem_finalize". Test `shmem.helpers.deaf.0-1`.
- **D22 - done.** `fatal.c`: the control segment line only while its name is in
  `/dev/shm`. Test `shmem.fatal.kept-segment-named`.
- Runs: related 72 of 72 with, 1 of 5 without; first suite 9 failures (seven the
  guard's, fixed; two mine from editing sources mid-run), then on the batch's tree
  alone: static 15 of 15, release 943 tests, 941 passed, 2 skipped, none failed.

### Batch 4 (`4f030c8`)

- **D2 - done.** `stat.c`: `list_segments`, `show_job` take every segment. Test
  `shmem.stat.view.many-nodes` (66 nodes): without "showed 64 of 66".
- **D3 - done.** `stat.c` `counters_human`: values for every PE. Test
  `shmem.stat.view.many-pes` (70 PEs): without "rows of 66 fields, not 72".
- **D5 - done.** `segment.c`/`.h` `osoe_segment_open_copy` (magic by `pread`, then
  a `pread` copy of at most a GiB); `gc_one` counts only failures. Tests
  `shmem.stat.gc.not-a-segment` (without: exit 1) and
  `shmem.stat.gc.shrunk-under-read` with a new shim, `shrink-under-read.c` (without:
  "Bus error", exit 135 - round four's `SIGBUS`, now deterministic).
- **D6 - done.** `stat.c` `process_stopped()`; PE line and summary markers. Test
  `shmem.stat.view.stopped`.
- **D7 - done.** `node.c` `wait_count`, `rendezvous.c`, `job.c` record `shmem_init`
  and the PE or node awaited; `test-job.c` stubs. Test `shmem.stat.view.init-hang`:
  without "0 of 3"; with the chain rank 1 -> PE 0 -> node 1 -> PE 3.
- **D8 - stopped** (see below); its README sentence made and checked.
- Runs: 6 of 6 with, 0 of 6 without; static 15 of 15; **release suite not run**.

## Where the tests stand at the end

- release, at `f122cc2` (batch 3): static 15 of 15; suite models aside 943 tests,
  941 passed, 2 skipped (`shmem.wire.zero-copy-refused`,
  `shmem.internode.zero-copy-refused.0-1-2`, root-only), none failed.
- release, at `4f030c8` (batch 4): built, 0 warnings; static 15 of 15; batch 4's
  six tests 6 of 6. The suite (now 949 tests models aside, 521 labelled
  `internode`) **not run**.
- Not run in this session at all: the debug, asan and tsan presets; the
  single-node build's suite; the model tests (no model changed); `selftest.py`
  (the engine did not change).

## What needs the author

- **D8, an owner's last pass with its age.** Options: (a) `last_pass_ns` in each
  owner record, stored on every pass, shown "passes N, the last 0.1 s ago" -
  layout 38, the metric table and its checksum, `osoe-stat`'s reader, the README;
  a few hours. (b) `osoe-stat` alone reads each owner's passes twice a quarter
  second apart and says "no pass in 0.25 s" - no layout change, every look a
  quarter second slower. (c) The README sentence - made in batch 4.
  **Recommendation: (a) at the next change that bumps the layout anyway; (b) if
  the author wants no layout change at all.**

## What the lead's merge needs

- **Close**: D2, D3, D5, D6, D7, D10, D11, D14, D15, D16, D18, D19, D20, D21, D22,
  D24, D25, D27, D28, D29.
- **Item text**: D8 - "stopped: needs a layout change (owner record field);
  README sentence made; options in cloud-session-4-response.md".
- **Re-run in the aarch64 VM** (from the merged tree):
  - `cmake --preset release && cmake --build build/release -j4`, then the static
    checks above and `ctest --test-dir build/release -j4 -LE model --timeout 300`
    (batch 4's suite was never run).
  - The debug and asan presets' suites (every batch), and tsan's: batch 2 added
    an atomic array in `transport-internode.c` and touched `progress.c`, batch 3
    the fatal/abort path's guard, batch 4 the blocked record during start-up.
  - The single-node build's suite (batches 1, 3 and 4 touched files it builds).
  - D27's sysctl branches by hand, as the manifest records, if the VM's kernel
    allows setting `kernel.io_uring_disabled`.
- **Record and README lines**: batch 4 moved the README's internode count to 521;
  batch 3's commit left it at 516 where it was 518. The README's "about 870 tests",
  "871 tests", "400 of the full build's" and "251 of the 470" were already of an
  older tree at `main` and were not touched.

## Decisions taken and deviations

- The branch is `cloud-session-4`, as the brief says.
- D16 a notice (rank 0, once a process), not a refusal; D24 once a PE; D11's
  agreement messages and D29's "prints nothing without a started job" left as
  they were.
- D5: a segment the listing found and `--gc` could not open is said on stderr and
  not counted; one named on the command line is counted.
- **Deviation**: batch 4 was committed without the release suite, and no
  sanitizer preset was run for any batch - the credit ran out. Batch 3's commit
  left the README's internode count one batch stale (corrected in batch 4).
- An unfinished draft of D30's helper (writing stderr in whole lines of at most
  `PIPE_BUF`) stayed in the session's scratch space and is not in the tree.

## Findings

- A `;` inside a ctest `PASS_REGULAR_EXPRESSION` or a list-passed pattern splits
  it into a list, and either half then passes: three existing tests
  (`shmem.refuse.no-shared-futex2`, both `shmem.dead-pe.no-pidfd.*`) passed on the
  wrong half until batch 2 replaced the `;` with `.`.
- Tests that compile the sources when they run (`shmem.static-link`,
  `shmem.strict-c11`, `shmem.single-node.configure`) and the `cmake -P` checks
  read the working tree: editing a source while a suite runs fails them
  spuriously. No source edits during a suite.
- Truncating a tmpfs object zeroes the rest of its last page, so a reader cut to
  64 bytes saw a zeroed directory and stopped cleanly; a cut to a page is what
  reproduces the `SIGBUS` (batch 4's shim).
- The README's suite counts named above are stale at `main`.

## What remains, by id

- Batch 5: **D23** (osoe-launch's start and timeout lines), **D30** (stderr in
  whole lines) - not started.
- Batch 6: **P8, P9, P10, P13, P16, P17, P21, P22, P25, P26, P27** - not started.
- Batch 7: **T18, T19, T22, T23, T31, T33, T34, T37, T40, T41, T43** - not started.
- **D8** - stopped, for the author (above).
- Outside the brief's batches and untouched: D1, D4, D9 and the rest of the open
  items.
