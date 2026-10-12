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

## Decisions taken that might have been the author's

*None yet.*

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

- **Close in `docs/prompts/v0.2-open-items.md`**: T42 (done, `cloud-session-1`).
