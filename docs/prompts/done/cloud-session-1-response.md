# Cloud session 1: the response

*The first cloud session's summary of its work, written on 2026-10-13 at the author's request.
The session followed `docs/prompts/cloud-session-1-prompt.md` on 2026-10-12, on the branch
`cloud-session-1` from `main` at `82c5263`. The full record is the manifest,
`docs/prompts/cloud-session-1-manifest.md`: every command, every pass count and the evidence for
each item. This file is the short version.*

## In short

- **The sandbox runs the full build whole.** The probe passes every line and the release build
  has no warning. Tiers 1 and 2 pass, internode tests included, over loopback with io_uring and
  futex2. No test failed in either tier.
- **All eight items are closed.** Six are changes, each shown catching what it was made for.
  B4 is documentation, and its move is put to the author. B5 needed no change: both fixes were
  already in the tree, and this session verified them. T32 leaves `allgather_model.py` to the
  lead, as the brief said.
- **Twelve commits came before this one, each pushed when made.** Nothing was force-pushed or
  merged, and no pull request was opened. No other branch was touched, nor any of the lead's
  files, and no new machine path entered the tree.

## The environment (Part 1, `e5033f7`)

The machine is a Firecracker microVM under KVM. It runs Ubuntu 24.04.5 on kernel 6.18.44, with
four vCPUs of an Intel Xeon, 15.7 GiB of memory, no swap and 30 GB of free disk.

- **Compilers**: GCC 13.3 is `cc`. GCC 14.2 (`gcc-14`), Clang 18.1.3 and an aarch64 cross GCC
  13.3 are installed beside it.
- **Tools**: CMake 3.28.3 and Python 3.13. liburing 2.9 is built from source by the setup
  script.
- **Nothing needed setting**: `/dev/shm` is 16 GiB, io_uring is enabled and `unshare -rn`
  works.
- **`ulimit -l`** is 8192 KiB and cannot be raised, because root here lacks `CAP_SYS_RESOURCE`.
  Root does hold `CAP_IPC_LOCK`, so the limit binds only the two tests that lower it to see a
  refusal. Both skip themselves by name.
- **Missing**: `ethtool`, which tier 3 needs and the mirror has, and an emulator for aarch64
  programs.

| Run | Result |
|---|---|
| Release preset | built at `-j4` in 49 s, **0 warnings** |
| The probe | **PROBE PASSED (0 failures)**: futex2, io_uring's futex wait, receive and zero-copy send, provided buffer rings |
| The static checks | **13 of 13** |
| Tier 1, cut at 30 minutes | 136 of 139 finished: **135 passed, 1 skipped, none failed**; the 3 unfinished models passed later, one at a time |
| Tier 2, models aside | **770: 769 passed, 1 skipped, none failed**, in 3 min 35 s, 449 of them internode tests |

The two skips are the refusal tests above. The probe passed, so the single-node build was not
needed.

**What a second session could do here**: C items with tests in the full build's `release`
preset. That covers library changes, tests of one node or of several logical nodes, the static
checks, and the models one at a time: the brief allows one per whole 8 GiB of memory, and this
machine has one. It could very likely run the `debug`, `asan` and `tsan` presets too, which
this session did not try, and tier 3 once `ethtool` is installed. It could not do the cluster's
or the Pi's runs, any measurement whose number is the result, aarch64 runs, or a test that
needs a process without `CAP_IPC_LOCK`.

## The items (Part 2)

| Item | Outcome | Commits |
|---|---|---|
| T42: a C23 compile test of the public headers | done | `bdcb86d` |
| B4: where the cache routines are declared | documented; the move is the author's | `7c4ba01` |
| B3: Example 48's plain profiling names in C11 | done | `52fa7cf` |
| B5: a Clang user's first command | fixed before; verified here with Clang 18 | `32230b4`, the manifest entry alone |
| P12: the shared atomics asserted lock-free | done | `5c853ad` |
| T30: a breakage only ORDER catches | done | `44e1975`, `a9f3877` |
| T32: model tests that name the property that fails | done but `allgather_model.py`, the lead's | `d86c959`, `a0c7bb3`, `e31fbb2` |
| B6: the cross build's nested checks find the target's liburing | done | `aaf55ba` |

**T42.** `shmem.headers.c23` (tier 1) joins the C99, C11 and C17 header tests. It compiles
`headers.c` at `-std=c23`, with every warning an error.

- It uses the build's compiler where that takes `-std=c23`, otherwise the first of `gcc-15`,
  `gcc-14` and `clang` that does. Where none does, it skips by name.
- Here GCC 13 refuses the flag, so the test runs `gcc-14`.
- The headers already pass, so the evidence is a mutant: `void shmem_barrier();` declared
  old-style passes the three older tests and fails the new one. C23 reads `()` as `(void)`.

**B4.** The six cache routines stay in `shmem.h`.

- No header satisfies §5 for them: it forbids them in `shmem.h`, and requires `shmemx_` names
  in `shmemx.h`. Moving them would also break the programs they are kept for.
- The README, `shmem.h` and `shmemx.h` now say where the routines are declared and why.
- `shmemx.h`'s citation of §7 is corrected to §5.
- Moving or dropping them is the author's call. The recommendation is to keep them.

**B3.** A profiler written as §10's Example 48 can now define `shmem_signal_set`,
`shmem_signal_add` and `shmem_sync` by their plain names in C11.

- `shmem.h`'s macros choose a form by argument count. Their context-free forms now pass their
  arguments through as written, so such a definition expands to
  `void (shmem_signal_add)(...)`, which defines the same function. A call is unchanged.
- `shmem.profiler-plain-names` (tier 1) compiles that shape at `-std=c11`, with every warning an
  error. Against the old headers it fails with M1's "expected declaration specifiers".
- The README, design §7.5 and `shmem.h`'s comments now describe this. The deprecated
  `shmem_wait` and `shmem_wait_until` on `long` still need parentheses, and the README says so.

**B5.** Both of round four's Q5 fixes were already in the tree: `test-reply.c`'s `second` is
initialized, and the nested strict-C11 build is given the outer compiler. So no code changed,
and the entry verifies the fixes with a Linux Clang, which the review lacked.

- `CC=clang` release build: 0 warnings.
- The release suite, models aside: 878 passed and 2 skipped of 880.
- With `CC=gcc` in ctest's environment, the nested build still uses Clang.
- A mutant with `second` uninitialized reproduces Q5's warning.

**P12.** `base/lock-free.h` checks that the atomics osoe shares between processes are
lock-free.

- At compile time, it refuses a compiler whose `ATOMIC_INT_LOCK_FREE`,
  `ATOMIC_LONG_LOCK_FREE` or `ATOMIC_LLONG_LOCK_FREE` is 0 ("never").
- Where a macro says "sometimes", `osoe_lock_free()` asks `atomic_is_lock_free`. `shmem_init`
  calls it beside the futex check, and refuses by name if the answer is no.
- Where every macro says "always", as GCC and Clang do on x86-64 and aarch64, the check
  compiles to nothing.
- It uses C11's own macros, and no memory order changed. §3.16's consequences now record this.
- The tests are `base.lock-free` and `base.lock-free.never`. The second fails without the
  assertion, and a scratch library answering `false` refuses a two-PE job by name.
- The 32-bit atomics are asserted too, not only the 64-bit ones the item names. That is one of
  the decisions listed below.

**T30.** `posted_bitmap_model.py` gains a breakage, `kept_bits`, that only ORDER catches. Its
agent loads a bitmap word instead of exchanging it, and keeps no record of the word it took, so
it forwards an arrival twice.

- It fails ORDER at 93 states. With ORDER's check switched off it holds, at 4,111,725 states.
- To make that so, the model's forward step now counts only real forwards. The design's state
  count is unchanged at 4,627,960.
- `take_arrival`'s comment in `internode-am.c` is corrected.
- The item has two commits because the first was pushed without the manifest entry its message
  claims. The second adds it.

**T32.** A model breakage now names the property it must fail, and the engine checks it.

- A check's message begins with the property it holds, as in `ORDER: ...`.
- `Result.failed` names the property a result fails. For STUCK, LOST WAKE and TRAPPED it is the
  verdict.
- `report(res, expect=...)` compares the two. A breakage that fails some other property prints
  `<-- UNEXPECTED: fails X, where it must fail Y` and fails the run.
- `selftest.py` holds the engine to this.
- Every model but `allgather_model.py`, 40 files, was converted by four Sonnet subagents
  working on separate files. This session reviewed every file.
- Each breakage names the property it fails now, so no verdict changed. The older `expect_ok=`
  still works, for `allgather_model.py`.
- The evidence uses T30's breakage with `wake_first`'s defect added: it fails STUCK. The old
  engine passed that case; the new one rejects it.

**B6.** The failure was reproduced in an aarch64 cross build. `shmem.strict-c11`'s nested
configure runs under ctest, which lacks the outer configure's `PKG_CONFIG_LIBDIR`, so it found
the host's x86-64 liburing ("file in wrong format").

- The build now hands `PKG_CONFIG_LIBDIR`, `PKG_CONFIG_PATH` and `PKG_CONFIG_SYSROOT_DIR`, as
  they were at configure time, to the nested configures of `shmem.strict-c11` and
  `shmem.static-link`. Those set each variable, or unset it where it was empty.
- The same cross build then passes with nothing set in ctest's environment. Natively, the
  static checks pass 13 of 13.

## Where the tests stand at the end

- **The release preset, models aside**: last run whole at P12's commit, with **882 tests: 880
  passed, 2 skipped**, the refusal tests above. After that commit:
  - T30 changed a comment in C.
  - T32 changed only the model checker and its models.
  - B6 changed how two nested checks are invoked.

  Each re-ran the static checks, 13 of 13, and the tests it touched.
- **The model suite**, `ctest -L model -j1` on the committed models: **39 of 39** in 9,274 s
  (2 h 35 min), with no result UNEXPECTED. `model.reservation` takes 2,167 s of its 2,400 s
  timeout here, so a slower core would time it out.
- **`get_completion_model.py`**, whose design cases no preset registers, run whole: all as
  expected, in 34 minutes.
- **With Clang 18**, at B5 before P12's two tests existed: 880 tests, 878 passed, 2 skipped,
  0 warnings.

## What needs someone else

**The author**: decide B4. The options are to keep the cache routines in `shmem.h`
(recommended), move them to `shmemx.h`, or drop them.

**The lead, at the merge**:

- Close T42, B4, B3, B5, P12, T30, T32 and B6 in `docs/prompts/v0.2-open-items.md`. As the
  brief asked, this session did not edit the open items.
- Give `docs/prompts/README.md`'s row for the brief, still "open", its outcome and merge
  commit.
- Convert `allgather_model.py`'s breakages to named properties, the same edit as the other
  models'. Until then, `report` treats its `False` as before.
- Two optional README lines:
  - once `allgather_model.py` is converted, that each model's breakages name the property they
    must fail;
  - that a Linux Clang has built the full build without a warning and passed its release suite.
- Run in the development VM what this sandbox could not verify. Each manifest entry ends with
  that list and the command for the VM. The main ones:
  - B6's cross build in the review's direction, an x86-64 build on aarch64, including
    `shmem.static-link`, whose program needs an emulator here;
  - P12's tests run on aarch64;
  - B5's Clang build, on aarch64.

## Decisions and deviations

Two decisions might have been the author's:

- **B4**: the cache routines stay in `shmem.h`.
- **P12**: the 32-bit atomics are asserted too. The futex words and the node's 32-bit protocol
  words are shared between processes the same way.

Deviations from the brief:

- `ulimit -l unlimited` was refused, so every suite ran under 8192 KiB, as root with
  `CAP_IPC_LOCK`.
- Tier 1's sample was cut at 30 minutes, by this session's limit on a background command, with
  three models unfinished. All three passed later in the `-j1` suite.
- T30 is two commits. The first was pushed without the manifest entry its message claims, and
  no force-push was used to mend it.
- T32 was committed while its model suite was still running, after the author remarked on the
  wait. The suite's result is its own commit, `a0c7bb3`, and so is `get_completion_model.py`'s
  run, `e31fbb2`.
- T32's conversion ran four subagents at once, so up to four of the smaller models ran together
  then. The model suite itself ran one model at a time.

## Findings, proposed and not fixed

- **A stale alarm in the probe.** Its 64-bit futex line prints "design 4.9 must change" on
  every Linux, though §4.9 changed long ago.
- **Stale test counts in the README.** It says "871 tests" and "470 labelled `internode`". At
  `82c5263` the counts were 878 and 476, and this branch brings the total to 882.
- **Docstrings that disagree with their cases.** Several breakages' docstrings name a property
  other than the one they fail. Each case now names what it does fail; the docstrings are
  unchanged.
- **STUCK or LOST WAKE depends on thread order.** At a dead end, the engine reports the first
  thread, by index, that is blocked or asleep, so a lost wake shows as STUCK whenever a blocked
  thread comes first.
- **Checks that no breakage reaches**, as ORDER was before T30. There are five, plus three
  guards, and the manifest names them all.
