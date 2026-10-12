# Cloud session 1: the manifest

*The record of the first cloud session (`docs/prompts/cloud-session-1-prompt.md`),
2026-10-12, on the branch `cloud-session-1` from `main` at `82c5263`, for the lead's
review after the author pulls. Written after Part 1 and after every item, each
time committed and pushed. Paths write the repository as `<repo>`.*

## Part 1: the environment

### The machine

| | |
|---|---|
| Kernel | `6.18.44-fc-v114` (`uname -r`), x86_64, a Firecracker microVM under KVM |
| Distribution | Ubuntu 24.04.5 LTS (Noble Numbat) |
| CPUs | 4 vCPUs, Intel Xeon ("Intel(R) Xeon(R) Processor @ 2.10GHz", family 6 model 207), one thread a core |
| Memory | 15.7 GiB (16,479,424 kB), no swap - one whole 8 GiB, so the models run one at a time (`-j1`) |
| Disk | 30 GB free |
| `ulimit -l` | 8192 KiB, soft and hard: **cannot be raised** (below) |
| `/dev/shm` | tmpfs of 16 GiB, writable: no remount needed |
| `unshare -rn true` | works (exit 0), and `unshare -n true` too |
| io_uring | `kernel.io_uring_disabled = 0`: nothing to set |
| Compilers | `cc` is GCC 13.3.0; GCC 14.2.0 as `gcc-14`; Clang 18.1.3; `aarch64-linux-gnu-gcc` 13.3.0 with `libc6-dev-arm64-cross` |
| Tools | CMake 3.28.3, Ninja 1.11.1, GNU Make 4.3, pkg-config 1.8.1, Python 3.13.16 (the image's `/usr/bin/python3`, through alternatives - not Ubuntu 24.04's 3.12), iproute2 6.1.0 (`ip`, `tc`), `sudo` |
| liburing | 2.9, under `/usr/local` - the setup script's build from source; Ubuntu's `liburing-dev` (2.5) is not installed |
| Missing | `ethtool`, which the `tier3` preset needs (the mirror has 6.7: `apt-get install ethtool`); `qemu-user`, so an aarch64 build's programs cannot run here |

**What was set, and what could not be.** Nothing needed setting: `/dev/shm` is
16 GiB and `kernel.io_uring_disabled` is 0. `ulimit -l unlimited` is refused
("cannot modify limit: Operation not permitted"), as root: the sandbox's root
holds every capability but `CAP_SYS_RESOURCE` (`CapEff` `000001fffeffffff`, bit 24
clear), so the hard limit of 8192 KiB stands, and every suite below ran under
it. Root does hold `CAP_IPC_LOCK`, under which the kernel charges neither
io_uring's rings nor a zero-copy send's pages to the limit, so it binds only a
test that lowers its own limit to see a refusal - and the two such tests skip
themselves by name ("SKIPPED: this process may lock memory", "SKIPPED: a PE may
lock memory"). That is the one difference running as root made.

### The build, the probe, the static checks

- **Release preset**: configured in 1.3 s; built at `-j4` in 49 s (2 min 41 s of
  CPU); **0 warnings**.
- **The probe** (`build/release/tools/probe/probe`), every line, exit 0:

```
info kernel 6.18.44-fc-v114 x86_64, page size 4096
info full build: liburing 2.9
info locked-memory limit (ulimit -l): 8192 KiB
ok   futex_wait, named shm, woken by other process woke=1 child_exit=0 (2: never woken, 3: open by name, 4: wait error)
ok   unlinked shm object reopened via /proc       child_exit=0 (3: open /proc/<pid>/fd, 4: map or read, 5: not live)
ok   futex2 U32 wait (value mismatch)             r=-1 errno=Resource temporarily unavailable
info futex2 U64 wait                              r=-1 errno=Invalid argument -> U64 NOT implemented (design 4.9 must change)
ok   uring futex_wait, named shm, woken by process woke=1 child_exit=0 (2: never woken, 3: open by name, 4: wait error, 5: ring)
ok   SINGLE_ISSUER + CQSIZE                       r=0 sq=64 cq=4096
ok   opcode FUTEX_WAIT                            
ok   opcode RECV                                  
info opcode SEND_ZC, for OSOE_WIRE_ZERO_COPY=on: supported
ok   uring futex_wait stale value -> -EAGAIN      res=-11
ok   shmem_init's io_uring check                  
ok   register provided buffer ring                ret=0
ok   TCP_NODELAY                                  
ok   multishot recv: one CQE per provided buffer  data=8 with_F_MORE=8 buffers=8
ok   buffer ring exhaustion -> one terminal -ENOBUFS cqes=9 last=-105
PROBE PASSED (0 failures)
```

  Every facility the full build needs is here: futex2, io_uring's futex wait,
  receive and zero-copy send, provided buffer rings. The single-node build was
  therefore not built for the sample (the brief's step 5).
- **The static checks**, `ctest --test-dir build/release -R
  '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' -j4`:
  **13 of 13** in 77 s, `shmem.strict-c11` 76 s of it.

### The suites, sampled

- **Tier 1**, `ctest --test-dir build/release -L tier1 -j4 --timeout 120`: 139
  tests. At the 30-minute bound **136 had finished: 135 passed, 1 skipped, none
  failed**; three models were still running and were stopped there -
  `model.reservation`, `model.deadlock_owners` and `model.shape_timing` - and ran
  later in T32's model suite (below). The 101 tests that are not models took 103
  s of test time, `shmem.strict-c11` 80 of it; the models, four at a time on four
  cores, took the rest: `model.abort` 1,461 s, `model.abort_exit` 1,298 s,
  `model.tree_barrier` 771 s, `model.close` 288 s, `model.posted_bitmap` 183 s,
  `model.lead_wake` 182 s, `model.allgather` 159 s, `model.ack` 156 s, and the
  other 26 under 135 s each. (`--timeout 120` changes nothing here: every test
  carries a `TIMEOUT` of its own, which ctest prefers.)
- **Tier 2**, `ctest --test-dir build/release -L tier2 -LE model -j4 --timeout 120
  --output-on-failure`: **770 tests, 769 passed, 1 skipped, none failed**, in 3
  min 35 s (774 s of test time; 449 of the tests are labelled `internode`, every
  one of which passed over loopback).
- **The failures, by cause**: none. io_uring refused, futex2 refused, locked
  memory, namespaces, timeouts, other - nothing in either tier. The two skips
  are the root case the brief foresaw: `shmem.wire.zero-copy-refused` (tier 1) and
  `shmem.internode.zero-copy-refused.0-1-2` (tier 2), which need a process that
  may not lock memory and skip by name under `CAP_IPC_LOCK`.
- **Not sampled**: tier 3 (`ethtool` is missing, and `netem` in this kernel was
  not tried), the `debug`, `asan` and `tsan` presets, and the single-node build -
  the probe passing, the brief's step 5 did not apply.

### What a second session could do here

The sandbox runs the full build whole: the probe passes every line, the
release build is warning-free in under a minute, and tier 1 and tier 2 pass -
the internode tests among them, over loopback with io_uring and futex2 - four at
a time, tier 2 in under four minutes. So a second session can take **C items
with tests in the full build's `release` preset** - library changes, new tests
of one node or of several logical nodes, static checks, the models one at a
time (one whole 8 GiB here) - and very likely the `debug`, `asan` and `tsan`
presets, which this session did not try; tier 3 needs `ethtool` installed from
the mirror first, and its `netem` shaping has not been tried in this kernel. It
cannot take what needs other machines or quiet ones: the cluster's runs and the
Pi's (the H items), any measurement whose number is the result (a shared KVM
guest of four vCPUs is not a measuring machine), aarch64 runs (no emulator; a
cross build compiles but its programs cannot run), and anything that needs a
locked-memory limit above 8 MiB for a process without `CAP_IPC_LOCK` - here
every process has it, so the two refusal tests can only skip. Python is 3.13,
and the default compiler GCC 13, whose C23 is `-std=c2x`.

## Part 2: the items

*Each item: done, partial or skipped, and why; its commits; the files it
changed; the exact commands run and their pass counts; its failing-without-the-
change evidence; and what this sandbox could not verify, with the commands for
the development VM.*

### T42: a C23 compile test of the public headers - done

*R9 P9-6 (`docs/reviews/archive/v0.2-before-part4-portability.md`): the headers
were tested under C99, C11 and C17 only, while GCC 15 compiles every program that
names no standard as `gnu23`.* Still open at `82c5263`:
`shmem/tests/CMakeLists.txt`'s loop ran `c99 c11 c17`.

- **The change**: `shmem.headers.c23` (tier 1, 30 s), beside the three -
  `headers.c` under `-std=c23 -Wall -Wextra -Wpedantic -Werror -fsyntax-only` -
  with the build's compiler where it takes `-std=c23` (CMake's
  `check_c_compiler_flag`), otherwise the first of `gcc-15`, `gcc-14` and `clang`
  that does; where none does it is registered all the same and skips, saying
  `SKIPPED: no compiler here takes -std=c23 (...)`. `headers.c`'s comment names
  C23. Here the build's GCC 13 refuses `-std=c23` ("did you mean '-std=c2x'?"), so
  the test runs `gcc-14`; in the development VM, Debian 13's GCC 14 takes it
  itself. Files: `shmem/tests/CMakeLists.txt`, `shmem/tests/headers.c`.
- **Ran**: `cmake --build build/release -j4` (0 warnings), then
  `ctest --test-dir build/release -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.|headers\.)' -j4`:
  **19 of 19** (the static checks and the six header tests).
- **The skip, shown**: a configure that can see no C23 compiler
  (`-DCMAKE_IGNORE_PATH="/usr/bin;/bin;/usr/local/bin;/usr/sbin;/sbin"` with the
  compiler and `make` given, the single-node build so that pkg-config is not
  needed) registers the test, and `ctest -R shmem.headers.c23` reports it
  `***Skipped` with its line.
- **Failing without the change**: the headers pass under C23 today, so the
  evidence is a mutant beside its control. With `shmem.h`'s `shmem_barrier`
  declared old-style, `void shmem_barrier();`, the three existing tests still pass
  (`cc -std=c99|c11|c17 ... headers.c`: exit 0 each - no test before this one
  could see it), and the new one fails: `gcc-14 -std=c23 ...` exits 1, "too many
  arguments to function 'shmem_barrier'" - C23 reads `()` as `(void)`. On the
  real header it passes (above).
- **Not verified here**: nothing beyond the release build; the VM's own run is
  `ctest --preset release -R '^shmem\.headers\.'`, where `shmem.headers.c23`
  should name GCC 14, the build's compiler, in its command (`ctest -V`).

### B4: where the cache routines are declared - done as documentation; the move is the author's

*Conformance M5 of round four (`docs/reviews/archive/phase1-release-fixes-conformance.md`;
the release checklist's "After the release"): the six cache routines, which left
the specification after 1.4, are declared in `shmem.h`, where §5 allows "only the
interfaces and constant names defined in this specification", and the README
"does not say where they are declared".* Still so at `82c5263`
(`shmem.h:939-946`; the README's limit "`shmemx.h` is empty" named no header).

- **What a move would cost** (read in the specification's source at the `v1.6`
  tag, `content/language_bindings_and_conformance.tex` and Annex F's table): §5
  also says every extension in `shmemx.h` "shall use the `shmemx_` prefix", so
  no header takes `shmem_clear_cache_inv` by the letter - `shmem.h` breaks §5's
  "only", `shmemx.h` its prefix. And a move breaks the programs the routines are
  kept for: with GCC 14 an undeclared call is an error, so every one would need
  `#include <shmemx.h>` added. Both reviews rated the placement harmless (round
  three's C4.8, round four's M5) and asked only that the README say it.
- **The change**: the README's limit now says where they are declared and why
  (`shmem.h`, where those programs look; §5's two rules); `shmem.h`'s comment
  over them says the same; `shmemx.h`'s says they are not there, and its
  citation, "(spec §7)", is corrected to §5 - §7 of 1.6 is "Library Handles";
  the `shmemx.h` rule is §5's. Files: `README.md`, `shmem/include/shmem.h`,
  `shmem/include/shmemx.h`.
- **Ran**: `cmake --build build/release -j4` (0 warnings); the static checks,
  `shmem.headers.*` and `shmem.profiling_aliases`: **20 of 20**.
- **Failing without the change**: none to show - comments and a README
  sentence, no behaviour.
- **For the author**: whether to move them to `shmemx.h` after all (or drop
  them). *Recommended:* keep them in `shmem.h` - the move trades one of §5's
  rules for the other and breaks the programs they are kept for.

### B3: Example 48's plain profiling names in C11 - done

*Conformance M1 of round four: a profiler written as §10's Example 48 is
(`example_code/pshmem_example.c` in the specification's source: `pshmem.h`
included, the intercepted routine defined by its plain name, forwarding to its
`pshmem_` spelling) cannot define `shmem_signal_add`, `shmem_signal_set` or
`shmem_sync` in C11, since `shmem.h`'s macros, which choose those routines'
forms by argument count, expand the definition ("expected declaration
specifiers").* Still so at `82c5263`.

- **The change**: the three macros' forms without a context - `OSOE_SIGNAL_SET_3`,
  `OSOE_SIGNAL_ADD_3`, `OSOE_SYNC_4` - pass their arguments to the parenthesized
  name as written instead of each in parentheses. A call is unchanged (each
  argument is already a whole argument of the call), and a definition or
  declaration by the plain name now expands to one of the name in parentheses,
  `void (shmem_signal_add)(uint64_t *sig_addr, uint64_t signal, int pe)`, which
  declares the same function. No change to `pshmem.h`: M1's own suggestion, a
  `pshmem.h` that drops the C11 macros, would break a program that includes it
  and calls a C11 form, as `headers.c` does. `shmem.profiler-plain-names` (tier 1)
  compiles `profiler-plain-names.c` - the three defined by their plain names,
  then both forms of each called - at `-std=c11` with every warning an error.
  The README's "C only" limit, design §7.5's paragraph on the signal updates and
  `shmem.h`'s comments say what is now true; the README also says what stays:
  the deprecated `shmem_wait` and `shmem_wait_until` on a `long`, whose names
  C11 gives the type-generic waits, a profiler still writes in parentheses (a
  generic selection cannot pass a parameter list through). Files:
  `shmem/include/shmem.h`, `shmem/tests/profiler-plain-names.c`,
  `shmem/tests/CMakeLists.txt`, `README.md`, `docs/design/shmem-ethernet-design.md`.
- **Ran**: `cmake --build build/release -j4` (0 warnings); the static checks with
  `shmem.headers.*`, `shmem.generic-arity.*`, `shmem.profiler-plain-names` and
  `shmem.profiling_aliases`: **26 of 26**; every test of the suite whose name
  matches `signal|sync|profile|surface|active-set|deprecated`, models aside
  (`shmem.signal.L1/L2/L4`, `shmem.profile.*`, `shmem.surface.*`, the active
  sets, the pacer's deprecated and signal plants): **49 of 49**. Also compiled
  with Clang 18 at `-std=c11` and GCC 14 at `-std=c23`; `nm` of the object shows
  `shmem_signal_add`, `shmem_signal_set` and `shmem_sync` defined (`T`) and the
  four-argument forms calling `shmem_ctx_signal_*` and `shmem_team_sync`.
- **Failing without the change**: `profiler-plain-names.c` against the headers
  of the commit before (`git show HEAD:shmem/include/...` into a scratch
  directory): exit 1, "error: expected declaration specifiers or '...' before
  '(' token" at `shmem.h:1544`, `OSOE_SIGNAL_ADD_3`'s expansion - M1's error;
  against the changed headers, exit 0.
- **Not verified here**: nothing that needs another machine. In the VM:
  `ctest --preset release -R 'profiler-plain-names|headers|signal|profile'`.

### B5: a Clang user's first command - done: both fixes were in the tree, verified here with a Linux Clang

*Round four's portability Q5 (`docs/reviews/archive/phase1-release-fixes-portability.md`):
`CC=clang cmake --workflow --preset release` failed `shmem.strict-c11`, on Clang's
`-Wsometimes-uninitialized` at `test-reply.c`'s `second`, and the nested build
took `CC` from the environment rather than the outer build's compiler; the
review had no Linux Clang to run.* Confirming in the tree: both fixes are there -
`second` is initialized (`shmem/tests/test-reply.c:275`, with a comment naming
Clang's warning) and `shmem.strict-c11` passes `-DC_COMPILER=${CMAKE_C_COMPILER}`
to `cmake/CheckStrictC11.cmake` (the v0.2 step 1 review's portability Q2). So
the item needed no change: this entry is its verification, and no code is
committed for it.

- **Ran, with Clang 18.1.3**: `CC=clang cmake --preset release -B build/clang-release`
  (the C compiler identified as Clang), `cmake --build build/clang-release -j4`:
  **0 warnings**; the static checks with the header tests, `ctest --test-dir
  build/clang-release -R '<the static checks>|headers\.|profiler-plain-names'`
  with `CC` unset: **20 of 20**, the nested build's cache naming `/usr/bin/clang`
  (`shmem.static-link`'s too); then the release test preset, every test but the
  models - the first command's test step - with `CC=clang` in the environment:
  **880 tests, 878 passed, 2 skipped (the root case), none failed**, in 5 min 38 s.
- **The nested build takes the outer compiler, shown**: with `CC=gcc` in ctest's
  environment, `shmem.strict-c11` of the Clang build passes and its nested cache
  still names `/usr/bin/clang`.
- **Failing without the fix**: a copy of the tree with `second` left
  uninitialized (`uint32_t ticket = 0, first = 0, second;`) through
  `cmake/CheckStrictC11.cmake` with `-DC_COMPILER=/usr/bin/clang`: exit 1,
  "Building with -std=c11 warned (F12)", `test-reply.c:282:11: warning: variable
  'second' is used uninitialized whenever '&&' condition is false
  [-Wsometimes-uninitialized]` - Q5's warning; on the tree as it is, it passes
  (above).
- **Not verified here**: Clang on aarch64 and Apple's Clang, which the review
  used; in the VM, `CC=clang cmake --preset release -B build/clang-release &&
  cmake --build build/clang-release -j4 && ctest --test-dir build/clang-release
  -LE '^model$' -j4`.

### P12: the shared 64-bit atomics asserted lock-free - done

*The step D review's portability Q4 (`docs/reviews/archive/phase1-step7-slice3-portability.md`):
`node.h` alone declares 46 `_Atomic uint64_t` fields that processes share, and
nothing asserts they are lock-free, though C11 expects atomics to be
address-free - two mappings of one object communicating - only where they are
lock-free; the futex words' size and alignment are asserted, their
lock-freedom is not.* Still so at `82c5263`: no `ATOMIC_*_LOCK_FREE` or
`atomic_is_lock_free` anywhere in the tree.

- **The change**, §3.16's ground - an assertion, no memory order changed:
  `base/lock-free.h` refuses, when compiling, a compiler whose `<stdatomic.h>`
  says `int`'s, `long`'s or `long long`'s atomics are never lock-free (`0`) - the
  types of `uint32_t` and `uint64_t` on LP64 Linux, `long long` too since a C
  library could make `uint64_t` that - and `osoe_lock_free()` answers `true`
  where it says always (`2`), and otherwise asks the objects themselves with
  `atomic_is_lock_free` (`1`, "sometimes", which only the machine can answer);
  `shmem_init` calls it beside the kernel's futex check, before anything is
  shared, and refuses by name. C11's own macros only, no builtin (§3.16's first
  rule). GCC and Clang say always on x86-64 and aarch64, so the start-up branch
  compiles to nothing here. §3.16's consequences say so. Tests (tier 1):
  `base.lock-free` builds `test-lock-free.c` with `long`'s atomics made
  "sometimes" and asks the objects (yes, on this machine); `base.lock-free.never`
  compiles it with them made "never" and must be refused by the header's
  assertion, by its message. Files: `base/lock-free.h`,
  `base/tests/test-lock-free.c`, `base/tests/CMakeLists.txt`, `shmem/src/init.c`,
  `docs/design/shmem-ethernet-design.md` (§3.16).
- **Ran**: `cmake --build build/release -j4` (0 warnings); the static checks and
  `base.lock-free*`: **15 of 15**; the release test preset, `ctest --test-dir
  build/release -LE '^model$' -j4`: **882 tests, 880 passed, 2 skipped (the root
  case), none failed**. Both test modes compiled and run with Clang 18 too.
- **Failing without the change**: `base.lock-free.never`'s compile against the
  tree before (`git archive HEAD base`): no header, so no assertion's message -
  the test fails; and against a mutant of the header without its
  `_Static_assert`: the "never" compile succeeds (exit 0), and the test fails.
  The start-up branch, shown on a running job: a scratch build whose
  `osoe_lock_free()` answers `false` refuses `osoe-launch -n 2 -- shmem_test_init`
  with "osoe: PE 0: osoe shares atomic words between processes, and this
  machine's 32- or 64-bit atomics are not lock-free: another process could not
  see their lock", exit 1, nothing left in `/dev/shm`.
- **Not verified here**: aarch64, whose GCC 14 says always as x86-64's does (by
  its `__GCC_ATOMIC_*_LOCK_FREE`; not compiled here). In the VM: `ctest --preset
  release -R 'base\.lock-free|static'`.

## Decisions taken that might have been the author's

- **B4: the cache routines stay in `shmem.h`**, documented where they are and
  why, rather than moved to `shmemx.h` - no header meets §5's letter, and a move
  breaks the programs that call them. The author may want them moved or dropped.
- **P12 asserts the 32-bit atomics too**, not only the 64-bit ones the item
  names: the futex words and the node's 32-bit protocol words are shared the
  same way, and the same C11 promise covers them.

## Deviations from the brief

- **`ulimit -l unlimited` could not be set** (Part 1): every suite ran under the
  hard limit of 8192 KiB, as root with `CAP_IPC_LOCK`.
- **Tier 1's sample was cut at 30 minutes** by this session's limit on a
  background command, not by choice: three models had not finished (Part 1).
  The brief bounded tier 2 at 30 minutes and tier 1 by its timeouts.

## Findings

*Stale, wrong or missing things met on the way that no item covers - proposed,
not fixed.*

*None yet.*

## For the lead's merge

- **Close in `docs/prompts/v0.2-open-items.md`**: T42 (done, `cloud-session-1`); B4
  (documented; the move put to the author, under Decisions); B3 (done); B5
  (fixed before, verified here with Clang 18); P12 (done).
- **A README line, if the lead wants it** (B5): under Requirements, that a Linux
  Clang, 18.1.3 on Ubuntu 24.04, has built the full build without a warning and
  passed its release suite (2026-10-12, `cloud-session-1`).
