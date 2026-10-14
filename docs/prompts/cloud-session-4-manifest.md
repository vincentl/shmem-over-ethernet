# Cloud session 4: the manifest

*The record of the fourth cloud session (`docs/prompts/cloud-session-4-prompt.md`),
2026-10-14, on the branch `cloud-session-4` from `main` at `7cd2d68`, for the lead's
review after the author pulls. Written after every item, each time committed and
pushed. Paths write the repository as `<repo>`. The response,
`docs/prompts/cloud-session-4-response.md`, is the complete account; this file is
the per-item log behind it.*

## The environment, since the third report

Unchanged from the third session's (`docs/prompts/cloud-session-3-response.md`, "The
environment"): Linux 6.18.44-fc-v114 on four vCPUs and 15.7 GiB, `/dev/shm` 16 GiB,
`kernel.io_uring_disabled = 0`, `vm.mmap_min_addr = 4096`, `ulimit -l` held at
8192 KiB for root with `CAP_IPC_LOCK` (`ulimit -l unlimited` is refused: "cannot
modify limit: Operation not permitted"), liburing 2.9, gcc 13.3 as `cc` with gcc 14.2
and clang 18.1 beside it, cmake 3.28.3; Python is now 3.13.16. The kernel still has no
netem (`# CONFIG_NET_SCH_NETEM is not set`). Nothing needed setting.

At `main` (`7cd2d68`): `cmake --preset release` and `cmake --build build/release
-j4` in 63 s, **0 warnings**; the static checks (`ctest --test-dir build/release -j4
-R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)'`)
**13 of 13**, in 98 s.

From batch 1 on, the static checks' expression gains `settings-known`, D16's new
check: `-R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|license-lines|single-node\.configure|version\.)'`.

**How "fails without" was shown, for every item below**: the batch's library change
was set aside (`git stash push --` the library's files, new library files moved out
of the tree) with its tests and checks kept, the build reconfigured and rebuilt, and
the batch's tests run with `--output-on-failure`; then the change was restored and
rebuilt. The lines quoted are from those runs.

## Batch 1: the settings' diagnostics

One commit for the four items: the one that adds this section.

### D16: unknown `OSOE_` names said, so a typo no longer leaves detection on - done

**The item** (RD, the round-three readiness report's diagnosability list; round
four's readiness, "A misspelt variable is silent"): `OSOE_DEADLOCK_SECOND=0`, meant
to turn the deadlock detector off, left it on, and a job waiting on a word was ended
as deadlocked at 10 s. Confirmed open: nothing read the environment for names it did
not know; only the three retired names drew a notice (`bootstrap.c`).

**What changed.**

- `shmem/src/unknown-settings.c` and `.h` (new): `known_settings[]`, every `OSOE_`
  name the tree reads or sets in a PE's environment - the library's settings (43),
  the launcher's names (10), the tools' (4), the retired (3) and the test suite's own
  (43) - 103 in all; `osoe_unknown_settings_known(name)`; and
  `osoe_unknown_settings_notice()`, which walks `environ` and, for each `OSOE_` name
  not in the list, says "`OSOE_DEADLOCK_SECOND is not a variable osoe reads, and
  changes nothing: did you mean OSOE_DEADLOCK_SECONDS?`" - the nearest known name by
  edit distance, at most two edits away - or, with none near, "`... (SHMEM_INFO=1
  lists the ones it reads)`". Once a process (a static flag: `shmem_test_init`
  starts the library three times), through `osoe_notice`, one write a line.
- `shmem/src/init.c`: `session_start` calls it on the job's rank 0, after
  `SHMEM_INFO`'s text - once for the job, as the retired names' notice is said.
- `cmake/CheckSettingsKnown.cmake` (new) and its test `shmem.settings-known` (tier 1,
  30 s): collects every `OSOE_` name read or set over `shmem/`, `base/`, `tools/` and
  `cmake/` - quoted literals in C, shell and Python files, `$OSOE_...` in shell, and
  `OSOE_...=` assignments in shell scripts and `CMakeLists.txt` (what tests and
  tools set for the PEs they start), with the launcher's `launcher_name("...")` -
  less the build's own CMake options (`option(OSOE_...)`, cache variables) and the
  two names the new tests set on purpose; and fails on a name read or set and not
  listed, and on a listed name nothing reads or sets.
- `cmake/CheckSettingsDocumented.cmake` leaves `unknown-settings.c` out of what it
  collects: the list holds the test suite's names, which the README does not.
- `shmem/CMakeLists.txt`: the new source.

**Wire or layout**: neither.

**Decision taken**: a notice, not a refusal. `base/environment.h`'s rule makes a
malformed *value* an error; an unknown *name* is said and ignored, as the retired
names are, so a script that sets a name a later osoe reads, or one osoe retired,
still runs. Said by rank 0 alone, as the retired names are: a name set on one other
PE only (`osoe-launch -e N:...`) is not said.

**Tests** (`shmem/tests/CMakeLists.txt`, tier 2, 90 s each):

- `shmem.settings.unknown.near`: `OSOE_DEADLOCK_SECOND=0`, two PEs of
  `shmem_test_init`; through the new `count-lines.sh`, exactly one line "`osoe: PE 0:
  OSOE_DEADLOCK_SECOND is not a variable osoe reads, and changes nothing: did you
  mean OSOE_DEADLOCK_SECONDS?`" and a clean job; none from PE 1.
- `shmem.settings.unknown.far`: `OSOE_FROBNICATE=1`, the line without a suggestion.
- `shmem.settings.unknown.known`: four known names of four groups set
  (`OSOE_DEADLOCK_SECONDS`, `OSOE_CC`, `OSOE_TEST_MAP`, `OSOE_BOOTSTRAP_TIMEOUT`), no
  such line, the retired name's own notice still said. A guard: it passes without the
  change, as it must.
- `shmem/tests/count-lines.sh` (new): runs a command and requires its success and
  exactly COUNT lines matching a pattern.

**Shown.** Without the change: `shmem.settings-known` fails ("no
<repo>/shmem/src/unknown-settings.c: nothing to check against"), `unknown.near`
("count-lines.sh: the command exited 0, 0 line(s) matching, wanted 1") and
`unknown.far` (its line not found). With it: all pass, `unknown.near` saying its
line once in three sessions. The check itself was shown to find what it is for: with
`OSOE_TEST_MAP` taken out of the list and `OSOE_NOT_READ_ANYWHERE` put in, it fails
naming both ("`OSOE_TEST_MAP (in shmem/tests/CMakeLists.txt)`" and
"`OSOE_NOT_READ_ANYWHERE`"); restored, "420 files, 103 OSOE_ names read or set, every
one of the 103 unknown-settings.c lists and no other". Its first run also found the
build's CMake options (`OSOE_TIER3=ON` in a message, `OSOE_MULTIPLE_MODELS=ON` in a
comment) and `osoe-launch`'s own test of `-e`, `OSOE_EXTRA`: the options are now left
out, and `OSOE_EXTRA` is listed.

**README**: the "Environment variables" opening says what an unknown name draws and
that `shmem.settings-known` holds the list to the tree.

### D24: a full context pool says `OSOE_MAX_CONTEXTS`, once a PE - done

**The item** (R9 Q5-8, `docs/reviews/archive/v0.2-before-part4-readiness.md`): 63
`shmem_ctx_create` calls succeed and the 64th returns nonzero with no word of
`OSOE_MAX_CONTEXTS`, where a split names `OSOE_MAX_TEAMS` and a thread
`OSOE_MAX_THREADS`; "a line under `SHMEM_DEBUG`, or once a PE, would tell the user
which variable to raise". Confirmed open in `context.c`'s `create`.

**What changed** (`shmem/src/context.c`): `create` notes whether the pool had no free
element (`full`), and the first time a process finds it so it says, after
`pool_mutex` is let go: "`shmem_ctx_create: no context free: OSOE_MAX_CONTEXTS is 3,
the contexts a PE may have at once, the default one included (design §4.11); the call
returns nonzero, as spec §9.5 lets it, and this PE says so this once`" (or
`shmem_team_create_ctx:`). `said_full` is a static under `pool_mutex`. A creation
that fails for memory, with an element free, stays quiet as before.

**Wire or layout**: neither.

**Decision taken** (the third session's finding 7 put it to the author; the review
offered the two forms): **once a PE**, not under `SHMEM_DEBUG` and not each call.
§9.5 makes a failed creation no error, and a program may probe for contexts by
creating them until one fails; a line a call would repeat with every probe, and a
line under `SHMEM_DEBUG` alone would not reach the user who needs it. The zero-copy
send's locked-memory notice is said once a process the same way. The author may
prefer either other form; each is a line's change.

**Test**: `shmem.context.pool-3.said-once.L2` (tier 2, 90 s): `OSOE_MAX_CONTEXTS=3`,
two PEs of `shmem_test_context`, which find the pool full twice each (the creations
to exhaustion, then one more); `count-lines.sh` requires exactly two lines naming
`OSOE_MAX_CONTEXTS is 3`. Without the change: "0 line(s) matching, wanted 2"; with
it, one line from each PE. A line a call would give four or more.

**README**: the `OSOE_MAX_CONTEXTS` row says a full pool returns nonzero and the PE
says so once.

### D27: the `EPERM` hint names the sysctl only where the sysctl refuses - done

**The item** (round four's Q1, `docs/reviews/archive/phase1-release-fixes-portability.md`):
under a container's seccomp profile, which answers `io_uring_setup` with `EPERM`, the
refusal said "which usually means the kernel.io_uring_disabled sysctl is set" of a
sysctl that read 0; the fix "have the message name a seccomp filter beside the
sysctl". Confirmed open: `transport-internode.c` named the sysctl on every `EPERM` at
creation, and the test `shmem.refuse.no-io-uring-setup` required that sentence of a
refusal that is `deny-io-uring`'s seccomp filter, on machines whose sysctl is 0.

**What changed.**

- `base/ring-check.c`, `.h`: `osoe_ring_sysctl_refuses(&value)` reads
  `/proc/sys/kernel/io_uring_disabled`: 0 allows; 2 refuses; 1 refuses a process
  without `CAP_SYS_ADMIN` (bit 21 of `CapEff` in `/proc/self/status` - no libcap)
  outside the group `kernel.io_uring_group` names (`getegid`, `getgroups`; -1, the
  default, names none) - the kernel's `io_uring_allowed`. -1 where it cannot be read.
- `shmem/src/transport-internode.c`, `osoe_internode_backend_check`: on `EPERM` at
  creation, by that answer - refuses: ", refused by the kernel.io_uring_disabled
  sysctl, which is N here" (at 1 adding "and admits only CAP_SYS_ADMIN and the group
  kernel.io_uring_group names, neither of which this process has"); allows: ",
  refused not by the kernel.io_uring_disabled sysctl, 0 here, which allows it, but
  by a seccomp filter, as a container runtime's default profile refuses io_uring";
  unreadable: ", refused by the kernel.io_uring_disabled sysctl or by a seccomp
  filter (the sysctl cannot be read here)". The buffer grew from 160 to 256 bytes.

**Wire or layout**: neither.

**Test**: `shmem.refuse.no-io-uring-setup` now requires the filter's wording, any
sysctl value that allows ("`[0-9]+ here, which allows it, but by a seccomp filter`"),
and fails on "sysctl is set" or "refused by the kernel.io_uring_disabled". Without the
change it fails, the job saying "(it fails with "Operation not permitted", which
usually means the kernel.io_uring_disabled sysctl is set)"; with it, "(it fails with
"Operation not permitted", refused not by the kernel.io_uring_disabled sysctl, 0 here,
which allows it, but by a seccomp filter, as a container runtime's default profile
refuses io_uring)".

**README**: the containers' paragraph, which said the message names the sysctl even
though it reads 0 inside the container, says it names the filter there.

### D29: `SHMEM_INFO` names every variable of the README's tables - done

**The item** (round four's minors, the readiness review's S5: "prints 21 of the
tables' 34 variables (none of the transport's buffers)"). Confirmed open: at `main`
the tables hold 47 names and the text named 28; missing were `OSOE_COLLECTIVE_PLACES`,
the transport's nine (`OSOE_AM_CHUNK`, `_BUFFER`, `_BUFFER_BUDGET`, `_CREDITS`,
`_HUGEPAGES`, `OSOE_WIRE_BYTES`, `_BUFFERS`, `_ZERO_COPY`, `_ZERO_COPY_BYTES`) and
the testing table's nine but `OSOE_HEAP_SHIFT` (`OSOE_FORCE_REMOTE`,
`OSOE_CHECK_QUIET`, `OSOE_TEST_PERTURB`, `OSOE_TEST_NOTICE_SECONDS`,
`OSOE_TEST_DEAF_HELPER`, `OSOE_TEST_ABORT_DELAY_MS`, `OSOE_WIRE_UNSENT_BYTES`,
`OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED`). The review's other half - nothing printed
without a started job - is how `SHMEM_INFO` works (`shmem_init` prints it) and was
left.

**What changed** (`shmem/src/init.c`, `print_info`): `OSOE_COLLECTIVE_PLACES` after
the shape, and two sections as the README has them, "The transport's buffers" and
"For testing", each entry with its range and default and its design section.

**Wire or layout**: neither.

**Test**: `shmem.version.info.every-variable` (tier 2, 90 s; in the static checks'
`version\.`) runs the new `info-every-variable.sh`: the names of the first column of
every table of the README's "Environment variables", each required as a whole word
in a one-PE job's `SHMEM_INFO=1` text. Without the change: "47 variables in the
README's tables, 19 not named"; with it, none.

**README**: "`SHMEM_INFO` prints most of this list" now says every variable of the
section's tables.

### Batch 1's runs

- release, rebuilt: **0 warnings**.
- The batch's tests (`ctest --test-dir build/release -j4 -R
  '^shmem\.(settings|version\.info|context\.pool|refuse\.no-io-uring|no-io-uring)'`):
  without the change **9 of 15** (the six above fail); with it **15 of 15**.
- The release suite, models aside (`ctest --test-dir build/release -j4 -LE model
  --timeout 300`): **936 tests, 933 passed, 2 skipped** (the root refusal tests,
  `shmem.wire.zero-copy-refused` and `shmem.internode.zero-copy-refused.0-1-2`, as in
  every session here), **1 failed: `shmem.boundary`**, in 306 s. The boundary check
  requires every library file in exactly one layer list, and the new
  `unknown-settings.c` and `.h` were in none; they are now the runtime's
  (`cmake/CheckBoundary.cmake`), like `fatal.c` and the sweep, and the check passes
  ("92 files above or on the boundary, none reaching a header below it").
- The static checks with `settings-known`, after that, on the batch's tree alone:
  **15 of 15** in 101 s - `main`'s 13, `shmem.settings-known` and
  `shmem.version.info.every-variable` (in `version\.`).
