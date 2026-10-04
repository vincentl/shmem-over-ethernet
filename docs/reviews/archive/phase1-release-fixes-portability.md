# Round four, question 4: will osoe run anywhere else?

Review of `main` at `08b3da0`, run 2026-10-03 against
`docs/reviews/adversarial-review-prompt.md`, at the merge point after the release
fixes (`docs/decisions/2026-10-02-release-fixes.md`, Parts 1 to 6). The previous
round's answer to this question is `phase1-release-portability.md`, at `b0e0b53`.
Labels: *documented* (a source says so: the tree, a kernel's or a vendor's
published text), *observed in source*, *measured* (run here, on this tree),
*recalled* (from memory, not checked).

## Machine and method

The `osoe-x86` Lima VM: Debian 13, kernel 6.12.95 arm64, 4 cores, 6 GiB, Apple's
Rosetta registered for x86-64 binaries (`rosettad` still stopped since the
previous round, so Rosetta translates just in time), `x86_64-linux-gnu-gcc` 14.2,
`liburing-dev:amd64` 2.9, Python 3.13, no Slurm, no clang. The review tree was
copied to the VM's `/tmp/q4r/tree` and built three ways, each with the `release`
preset's flags (`RelWithDebInfo`): cross-built for x86-64, natively for aarch64,
and natively for aarch64 with `-mno-outline-atomics`, so that every atomic is a
load-exclusive/store-exclusive loop, as on cores without LSE. Every build and run
held `/tmp/osoe-vm.lock`, four tests at a time where the suite ran. Results under
Rosetta are an x86-64 user space with x86-TSO ordering on an aarch64 kernel: not
an x86-64 kernel, and not an Intel or AMD memory system. Clang was run on the host
(Apple clang 21.0.0) over the tree's own compile commands with the VM's Linux
headers, as the previous round did. Nothing was run on the Pi cluster.

The earlier, interrupted start of this review left an x86 run in the VM
(`/tmp/q4`) that is void: its `LD_PRELOAD` named `.../shim/lib/$LIB/...`, a path
that does not exist, so every x86-64 process ran without the futex2 stand-in and
was refused at `shmem_init`. It was removed; nothing here rests on it.

## Verdict

**It runs where the README says it does, and almost everywhere else it now says
why it does not - but the README does not yet say where "elsewhere" begins.**
The release fixes hold in this question's area: start-up names the peer, the
address, the ports and the error it is waiting on (N1); the rendezvous acts on
nothing an unproved caller claims, and 15 callers holding the port open do not
delay a real node (N2); the README is right about one-node jobs without io_uring
(N3) and about architecture (X1, X2). Three runs found the library sound where it
had never run: this tree built for x86-64 passes every one of its tests that
Rosetta can run (347, and every one of the 317 others is Rosetta's or the cross
build's); a build whose every atomic is LL/SC, as on a Pi 3 or Pi 4, passes the
whole suite and three minutes of the stress tester's `mix`; and the 22 multi-node
Slurm environments the Pi cluster captured last round parse to the layout their
distributions define. By reading, a node's page size enters only node-local
layout, so 4 KiB and 16 KiB nodes can share a job.

What a stranger off the author's path meets, ranked below: on RHEL 10 and its
rebuilds - the current enterprise generation, which meets every version floor -
and in containers under the default seccomp profiles, io_uring is denied, and
the README's first command then reports 311 of its 664 tests failed, with
nothing in the README to say why (Q1); `OSOE_AM_HUGEPAGES=1` can never take
effect on any Linux, though the README promises huge pages (Q2); and the x86-64
network path has still never run, now the one large gap (Q3). **Nothing here
blocks release; Q1 and Q2 are README sentences to fix before it**, the rest is
minor or after release.

## The previous round's findings, checked as claims

| Previous | What was asked | On this tree | Evidence |
|---|---|---|---|
| N1, fix before release | a node that cannot reach a peer says so | **holds**: after 30 s and every minute, one line naming the node, its host-list entry, the address, the ports and the last error; a no-progress end before the first notice still names neither address nor error (Q8) | *measured* (below, Trust run 5: "node 0 (127.0.0.1, at 127.0.0.1) ... on its candidate ports in OSOE_PORT_RANGE=17000-26999: Connection refused"); the record's tier-3 test of a wrong address |
| N2, fix before release | prove the secret before acting on a hello; an unconfirmed caller must not hold the loop; a Trust paragraph | **holds**, each property and each sentence of the Trust paragraph | *measured*, five runs below; *observed in source* `listener.c:440-509` |
| N3, README wording | `env.probe` failing does not mean nothing runs | **holds**: the README says a one-node job runs without io_uring | *measured*: under Rosetta, which refuses io_uring, one-node jobs run (the 347 passes below) and multi-node ones are refused by name; so too natively under `deny-io-uring` (Q1) |
| N4, after release | fixtures from documentation; the 33rd `shmem_init` in a minute; one bound address; IPv6-only; `n[]`; stale `OSOE_PROCID` | the Pi's captures exist and parse right but are not yet fixtures; the 33rd initialization **still fails** (Q7); the README now says a name must resolve to an address the other nodes reach, and lists IPv6-only as refused; the last two not re-checked | *measured* (22 captures, 40 initializations); *documented* (README 233-241, 573) |
| N5 notes | the futex2 message; seccomp's `EPERM` named as the sysctl | **both unchanged** (Q6, Q1) | *measured* |
| X1, fix before release | name the architecture tested | **holds**: README 91-95 and 581-589, true as written; it describes the tree of 28 September and can now describe this one | *measured* below |
| X2, README line | nodes must share an architecture | **holds**: README 590-597; its list of what the rendezvous compares matches `struct agreement` (`rendezvous.c:35-48`) exactly | *observed in source* |
| X3, after release | the suite cannot see an x86-only lost wake | not re-run | - |
| minor | `global-exit.fatal.*` passed falsely where every PE refused | **fixed**: those tests now require the caller's own line, and fail under Rosetta instead of passing | *measured* |
| minor | `pacer-shim.c` generated by two targets | **fixed** (Part 1) | *documented* |
| minor | three tests use the host's compiler, not the build's | **unchanged**; `shmem.strict-c11`'s case now fails a Clang user (Q5) | *observed in source* |

## New findings, ranked

### Q1. Where io_uring is denied by default - RHEL 10 and its rebuilds, and containers - the README's first command reports 311 of 664 tests failed, and the README does not say why (fix before release: a README paragraph; high)

- **Where**: README "Requirements" (79-103) and "Building" (105-146);
  `shmem/src/init.c:290-301` (the refusal's text); the suite's multi-node tests.
- **The platforms** (*documented*): Red Hat's RHEL 10.0 release notes list
  io_uring as a Technology Preview, "By default, this feature is disabled in
  RHEL 10"; its rebuilds (Rocky, Alma) follow. EL10's stream carries liburing
  2.12 (CentOS Stream 10's `liburing-2.12-1.el10`, remirepo's package table)
  over Linux 6.12 (*recalled*), so EL10 meets both of osoe's floors: it
  configures, builds and runs one-node jobs. Docker's default seccomp profile
  does not allow `io_uring_setup` (its default action answers `EPERM`; moby PR
  46762) while it allows `futex_wait`, `futex_wake`, `pidfd_open` and
  `sched_setattr` (`moby/profiles` `seccomp/default.json`, fetched today);
  Podman's and Kubernetes' runtime-default profiles block io_uring likewise
  (*recalled*).
- **Scenario** (*measured*): the native build's suite, models left out, run
  under the tree's own `deny-io-uring setup` helper, which refuses
  `io_uring_setup` with `EPERM` exactly as the sysctl and those profiles do: **311
  of 664 tests fail** - 298 refused at `shmem_init` with "osoe needs io_uring with
  an io_uring created SINGLE_ISSUER, R_DISABLED and CQSIZE for a job of 2 nodes
  (it fails with "Operation not permitted", which usually means the
  kernel.io_uring_disabled sysctl is set)", and 13 that open a ring themselves.
  A stranger on Rocky 10 who follows the README sees half the suite fail; in a
  container the message points at a sysctl that is 0.
- **What the README says**: the Requirements name the sysctl generically (81-83)
  and the distributions that provide everything (Debian 13, Raspberry Pi OS
  Trixie) or fall short (Debian 12, Ubuntu 24.04, Rocky 9); "Building" says a job
  on one node still runs where io_uring checks fail. It names neither EL10 nor
  containers, and says nothing of the suite on such a machine. The library's
  behaviour is right: refused by name, before anything is made; the one-node
  path runs (Rosetta's run below shows the same with `ENOSYS`).
- **Fix**: before release, a paragraph: on RHEL 10 and its rebuilds io_uring is
  off by default, and in containers the default seccomp profile refuses it; there,
  one-node jobs run, a job of more than one node needs an administrator's sysctl
  (or a profile that allows io_uring), and the suite's multi-node tests fail at
  `shmem_init`, about 300 of them. After release: let `ctest` skip the multi-node
  tests when the ring check fails (a fixture, or `SKIP_RETURN_CODE`), and have the
  message name a seccomp filter beside the sysctl.

### Q2. `OSOE_AM_HUGEPAGES=1` can never take effect: the kernel refuses `MAP_HUGETLB` on a `/dev/shm` object, whatever huge pages are reserved (fix before release: the README row and design §3.4's table; high)

- **Where**: `shmem/src/connection-area.c:345-354`; the area is created by
  `osoe_shm_create` (`shm-object.c:37`, `shm_open`, so tmpfs); README 896
  ("`1`: map the node's connection buffers with huge pages, falling back, with a
  warning, when there are none"); design §3.4's table (465) and text (491-495).
- **Mechanism** (*documented*, Linux v6.12 `mm/mmap.c`, `ksys_mmap_pgoff`): for
  a file-backed mapping, `if (is_file_hugepages(file)) ... else if (flags &
  MAP_HUGETLB) return -EINVAL` - the flag is refused for any file not on
  hugetlbfs, before any huge page is looked for.
- **Evidence** (*measured*): a probe program on this kernel (`HugePages_Total` 0):
  `MAP_SHARED | MAP_HUGETLB` on a `shm_open` object fails `EINVAL`, the same flags
  on an anonymous mapping `ENOMEM` - two different refusals, the first not about
  supply. A two-node job with `OSOE_AM_HUGEPAGES=1`: every PE prints "the
  connection area could not be mapped with huge pages (Invalid argument); using
  ordinary pages", and runs. With pages reserved the anonymous mapping would
  succeed and the tmpfs one would still fail, by the code above. No test can see
  whether huge pages were used. The previous round read this fallback as "no
  huge pages"; the cause is the object's file system.
- **Fix**: the README row and the design's table say the setting has no effect
  yet; after release, an area on hugetlbfs (a named file under a hugetlbfs mount,
  or `memfd_create(MFD_HUGETLB)` reopened through `/proc`, which the probe already
  checks works), or `madvise(MADV_HUGEPAGE)` on the tmpfs mapping where
  `shmem_enabled` allows it.

### Q3. The x86-64 network path has still never run (after release, or before at an hour's cost; the gap, not a defect)

This tree's one-node paths pass on x86-64 under Rosetta (section below), and the
README says truthfully that "no x86-64 process has sent a message to another
node". Judged as a risk: low. The path has no code that depends on the
architecture (*observed in source*: no inline assembly; one architecture
conditional, `base/cacheline.h`; wire and rendezvous structures of fixed-width
fields; the internal region laid out by constants "rather than the compiler's
cache line", `internal-region.h`; io_uring's ABI is the same on both LP64
targets), and the x86 build's exported surface is the aarch64 one's to the
name. What it leaves untested on x86-64: the io_uring engine and connection
area under a real x86 kernel's scheduler, `env.echo-uring`'s two timing pins
(checked in every release build, set on aarch64 VMs; Part 6 saw one fail under
load), and every performance number. The run that settles it is in "Runs the
lead could make elsewhere" (1): any x86-64 host or cloud VM with Debian 13, for
an hour.

### Q4. Root's `osoe-stat --gc` trusts objects any local user can write: one file of garbage makes it exit 1, and truncating one mid-read kills it with `SIGBUS` (minor; a README sentence before release, the reader after; high)

- **Where**: README 969-980 ("`osoe-stat --gc` ... removes only what its user
  may, so a site runs it as root on each node"); `tools/stat/segment.c:241-368`
  (the segment is mapped `MAP_SHARED` and read in place; e.g. `osoe_segment_read`
  checks `m->offset` and then reads it again from the mapping, 451-453).
- **Evidence** (*measured*, as an ordinary user parsing objects it owns, which
  root's `--gc` would equally parse): a 4 KiB object of random bytes named
  `osoe.999998.0.0.0` makes `osoe-stat --gc` exit 1 ("is not an osoe control
  segment"), deterministically; a copy of a live segment, kept because its PEs
  live, truncated and restored in a loop while `--gc` ran 400 times: 5 runs died
  of `SIGBUS` (status 135), 217 exited 1, 178 exited 0. Part 1's read made `--gc`
  remove nothing it should not (names and owners checked); it does not make the
  parse robust to the file's owner.
- **Why it matters elsewhere**: a site that puts `--gc` in a Slurm epilog, which
  the README's "as root on each node" invites, gives every user a way to fail the
  epilog, and Slurm drains a node whose epilog fails (*recalled*). An object at
  rest is bounds-checked before every read; one its owner changes during the read
  can make `osoe-stat` fault, or read a field from the wrong place within its own
  address space. It writes nothing it parses, so this is a denial of service, not
  an escalation (*observed in source*).
- **Fix**: before release, one sentence: run `--gc` from a timer, not the
  epilog, whose safe line is the README's own `rm -f /dev/shm/osoe.${SLURM_JOB_ID}.*`.
  After release: read a segment by `pread` into private memory, and let `--gc`'s
  status count only what it failed to do.

### Q5. A Clang user's first command fails `shmem.strict-c11` (minor; medium-high)

The README offers "GCC or Clang" (85). `cmake/CheckStrictC11.cmake` configures its
nested build with no compiler, so it takes `CC` from the environment, and fails
on any `warning:` in it (53-55); `CC=clang cmake --workflow --preset release`
passes `CC` to that nested build. Clang warns once over this tree's 276
translation units, for both targets: `shmem/tests/test-reply.c:278:11: variable
'second' is used uninitialized whenever '&&' condition is false
[-Wsometimes-uninitialized]` (*measured* with Apple clang 21 and the project's
flags; the previous round found the same with the same method). Not run with a
Linux clang (none in the VM). Fix: initialize `second`; pass the outer build's
compiler to the nested configure, as `CheckStaticLink.cmake` does.

### Q6. A sandbox's or emulator's refusal of futex2 is reported as an old kernel (minor; previous round's, unchanged)

`init.c:282-287` says "osoe needs Linux 6.7 or later, for the futex2 calls (they
fail with "%s"); this kernel is %s" for any error. *Measured* under Rosetta: "(they
fail with "Function not implemented"); this kernel is 6.12.95+deb13-cloud-arm64".
The same text would meet a container whose seccomp profile predates the futex2
calls, or gVisor. Fix: name the kernel's age only when the release is below 6.7,
and a filter otherwise. The probe's "info futex2 U64 wait ... -> U64 accepted" on
`ENOSYS` is also unchanged.

### Q7. The 33rd initialization within a minute of a multi-node job fails, and the Limits do not say so (minor; previous round's N4, the mechanism deferred on purpose)

*Measured*: a program that runs `shmem_init`/`shmem_finalize` 40 times on two
logical nodes completes 32 sessions and fails the 33rd within 2 s: "node 0 found
none of its 32 candidate ports free in OSOE_PORT_RANGE=17000-26999; choose a range
this host does not use" - the job's own earlier sessions hold them in `TIME_WAIT`
(`listener.c:288-292`). The specification allows re-initialization (§9.1.1, "may
be re-initialized with a subsequent call to an initialization routine"). The
record deferred `SO_REUSEADDR` after release, which still holds; what does not is
the README's promise of "every limit a program can meet" (472-474) without this
one, and a message whose advice does not fit the cause.

### Q8. Minor notes

- **The Pi's multi-node captures are evidence, not fixtures.** The 22 environments
  of `docs/design/evidence/pi-cluster/2026-09-28-review/slurm-env-*` replay right
  (below), but `shmem/tests/slurm-fixtures/` still holds the provisional
  `written-*` ones its README says a capture should replace.
- **Long messages tear when PEs print at once.** glibc 2.41 writes an `fprintf` to
  unbuffered stderr in pieces of at most 128 bytes (*measured*: one write up to
  123 bytes, two at 130 to 203, three at 303). `osoe_fatal` and `osoe_notice` write
  once (`fatal.c:68`, `:122`), but 17 messages use `fprintf(stderr, ...)`; the
  pidfd fallback's notice, about 132 bytes and printed by every PE, interleaved
  mid-line under Rosetta. `SHMEM_DEBUG`'s lines, 119 bytes, did not.
- **Where `pidfd_open` is refused, a global exit's carrier counts a zombie as
  alive** (`liveness.c:183-187`, `kill(pid, 0)` until `ESRCH`), so a launcher that
  reaps slowly holds the end to the 20 s or 30 s bound. `osoe-launch` reaps at
  every `SIGCHLD` (`launch.c:476`) and `slurmstepd` reaps its tasks (*recalled*);
  under Rosetta, which forces this path, `shmem.global-exit.kills.L4` passed.
- **Ubuntu 24.04 LTS is excluded by one inline helper.** The 2.6 floor is right
  (*documented*: liburing's CHANGELOG puts "Add futex support" in 2.6), and the
  library needs nothing newer at run time than liburing's `LIBURING_2.4` symbols
  (*measured*, `objdump -T`). The only 2.6 call is `io_uring_prep_futex_wait`
  (*observed in source*), one SQE's fields; filling them by hand where the helper
  is missing would admit the most common x86-64 LTS without bundling anything.
  A challenge to an accepted trade-off (design §1), for the author.
- **Below 30 s, `OSOE_NO_PROGRESS_TIMEOUT`'s end names no address or error.** A
  node whose peer never answers, with the timeout at 5 s, ends with "node 0 has
  not answered node 1 on any of its 32 candidate ports in
  OSOE_PORT_RANGE=17000-26999" (*measured*, `listener.c:819-824`): the address
  and last error that N1's notice gives are in hand (`tried`) but not in the
  fatal line, and the notice comes only at 30 s. At the README's suggested 60 s
  the notice comes first.
- **A library built on Debian 13 needs glibc 2.38 on every node** (`__isoc23_strtol`
  and two more, *measured*): a site mixing distributions between build and compute
  nodes meets a loader error, loud. The README's "install ... the same on each"
  covers it.

## x86-64

- **Build** (*measured*): 0 warnings, 29 s at `-j4`, 688 tests registered; every
  `_Static_assert` holds (the build would fail otherwise), the cache line 64 there
  against aarch64's 128.
- **Suite**, the 664 that are not models: **347 passed, 317 failed, every failure
  Rosetta's or the cross build's**, each classified by its own output: 295
  multi-node tests refused at `shmem_init` by name ("(it fails with "Function not
  implemented")" - the `ENOSYS` text, rightly without the sysctl hint), 11 that
  open a ring themselves (`shmem.wire`, `.wire-pair`, `.barrier-am.*`,
  `.progress*`, `env.probe`, `env.echo-uring`), 9 that wrap the job in a seccomp
  filter (Rosetta answers `seccomp()` with `EINVAL`), `shmem.static-link` (its
  nested build makes an aarch64 archive) and `tools.pacer.installed` (`ldd` of
  another architecture). The 347 were run a second time verbosely: all passed, and
  none printed an io_uring refusal, so none passed by being refused.
- **What that covers that is new since the previous round**: Part 2's global-exit
  waits on the `kill(pid, 0)` path, since Rosetta refuses `pidfd_open`; Part 3's
  listener - the TCP rendezvous (`shmem.job.*`) and the secret proved before any
  claim is judged (`shmem.job.secret*`, `shmem.listener.secret`); start-up
  refusals that publish to the node (`shmem.start-up.own-environment.*`,
  `.tombstone-gc`, `.same-ids`, `.lead-ends-in-join`, `.never-calls-init.*`); the
  "blocked in" record's four words (`shmem.blocked-record`); the stress tester's
  one-node exercises (`tools.pacer.0-0-0-0`, `.threads.0-0-0-0`).
- **Install and `oshcc`** (*measured*): `cmake --install` of the x86 build puts in
  the prefix exactly what README 169-171 lists; the installed `oshcc` builds the
  README's ring, linked with `RUNPATH` to the prefix, and it prints the right four
  lines at 4 PEs; a program built `-mcmodel=medium`, whose 1 MiB arrays land in
  `.ldata` and `.lbss` - the sections only x86-64's part of `osoe-statics.ld`
  gathers - has both inside the bracketed ranges, and each PE's puts into the next
  PE's copies arrive (3 PEs).
- **Exports** (*measured*): 3,595 defined dynamic symbols on x86-64 and aarch64
  alike, the same names: 1,794 strong `pshmem_`, 1,787 weak `shmem_`, the legacy
  `start_pes` family; no `osoe_` name.
- **Not covered**: anything between nodes (Q3), an x86-64 kernel's address space
  (the heap candidates fit 47 bits, *observed in source*), Intel and AMD memory
  systems, and the x86-only lost wake (X3), not re-run.

## aarch64 beyond the development machines

- **Cores without LSE** (Pi 3 and 4: Cortex-A53, A72). The library is built with
  outline atomics (*measured*: 100 calls to `__aarch64_*` helpers), which take
  LSE or an LL/SC loop at run time; no machine of the project has run the second.
  Built with `-mno-outline-atomics` (200 load- and store-exclusive instructions,
  no LSE instruction, no helper call), the suite passed **664 of 664**, and
  `osoe-pacer mix` ran 180 s on two logical nodes of four PEs with `failures=0`,
  2.5 GB moved between them (*measured*). That is the LL/SC instruction sequence
  on Apple's cores, not a Cortex-A72's timing.
- **Page sizes**. The page size enters only node-local layout (*observed in
  source*): `page_round` in `node.c` sizes a node's own heap object and where its
  internal region begins, while what crosses nodes is addressed by region and
  offset, the internal region's offsets fixed by constants, and the rendezvous
  compares the raw `SHMEM_SYMMETRIC_SIZE`. A job of 4 KiB nodes (a Pi 4) and
  16 KiB nodes (a Pi 5) is consistent by construction; not run.
- **Address-space sizes**. The heap-base candidates cover 48-, 42- and 39-bit
  user spaces (`node.c:52-57`), and a lead that finds none free refuses by name.

## Kernels and sandboxes

| Facility | What osoe does without it | Evidence |
|---|---|---|
| futex2 (6.7) | refused at `shmem_init` by every PE, before anything is made; the message claims an old kernel whatever the cause (Q6) | *measured* (Rosetta, no shim) |
| io_uring, `ENOSYS` (no `CONFIG_IO_URING`, emulators) | one-node jobs run; multi-node refused by name, no sysctl hint | *measured* (Rosetta) |
| io_uring, `EPERM` (the sysctl; seccomp) | the same, the hint naming the sysctl even under seccomp (Q1) | *measured* (`deny-io-uring`) |
| `pidfd_open` | liveness by `kill(pid, 0)` with one notice a PE; global exit's carrier waits the same way (Q8) | *measured* (Rosetta) |
| `MAP_HUGETLB` on the area | always refused (Q2) | *measured*, *documented* |
| `RLIMIT_MEMLOCK` | not needed: soft and hard 64 KiB and 0 both ran a two-node job, and the probe passed at 0 | *measured* |
| the agent's ring | sized `2(n-1)+1` submissions and `buffers + 2(n-1) + 1` completions, inside the kernel's 32,768 and 65,536 for any job the 1,025-node buffer budget admits; a refusal is named | *observed in source* (`ring.c:41-54`) |

The probe now makes `shmem_init`'s own ring check (Part 6), so `env.probe` and the
library refuse alike; its line for the build's liburing (`IO_URING_VERSION_*`) is
the header's, not the node's runtime package.

## Slurm

- **The bootstrap changed by one variable**: `SLURM_RESTART_COUNT` (or
  `OSOE_LAUNCH`) tells a leftover's launch from this one; unset reads as 0
  (`bootstrap.c:112-127`). Nothing else in the Slurm path changed, so the previous
  round's runs - every distribution and binding the VM's Slurm 24.11 could run
  or `osoe-launch -s` emulate, batch and `salloc` shells refused by name - stand.
- **Real multi-node environments** (*measured*): the 22 the Pi cluster captured
  last round - block 4 × 2, cyclic 4 × 2, and an uneven step of 6 tasks as 3, 1,
  1, 1 - each agree with what their distribution defines, and each, replayed
  through `shmem_test_bootstrap`, gives that layout: **22 of 22**. That closes
  the previous round's "no multi-node environment has ever been captured" (Q8:
  promote them).
- **The README's batch script**: its secret line yields 32 hexadecimal digits with
  coreutils' `od` (*measured*). The script itself has not run on a cluster (the
  release checklist has it).

## Build and install

- **Requirements**: the 2.6 floor is justified (Q8); CMake refuses an older
  liburing at configure by name (`CMakeLists.txt:51-57`); 32-bit targets fail at
  compile time (`lock.c:83`, `sizeof(long) == 2 * sizeof(uint32_t)`), which the
  README does not mention (it says aarch64 only).
- **The wrapper** finds its tree from where it is installed and takes the library
  directory from `CMAKE_INSTALL_LIBDIR`, so a `lib64` distribution is handled
  (`tools/wrapper/CMakeLists.txt:37-50`, *observed in source*); `-static` is
  refused by name (`wrapper.show`). The linker script's own comment says gold and
  mold refuse `INSERT`; the README does not.
- **Compilers**: GCC 14 gives 0 warnings for both targets; Clang one, in a test
  (Q5).

## Trust

Five two-node jobs, node 0 started alone with `OSOE_TEST_NOTICE_SECONDS=2` and
`OSOE_NO_PROGRESS_TIMEOUT=25`, a caller of my own at its port, then, 1.5 s later,
the real node 1; times are from node 0's start to the job's end (*measured*; the
driver `trust/run.sh` and `hello.py` below):

1. **With the secret, a hello claiming 10⁶ tasks on 10⁶ nodes**: answered
   ACCEPTED, then "node 0 refused a caller claiming to be node 1 of this job: it
   does not know OSOE_JOB_SECRET"; node 0 lived, and the job ended, both PEs
   joined and status 0, at 1.6 s.
2. **With the secret, a claim to be node 7 of the two**: the same.
3. **With the secret, 15 callers answered and holding**: the job ended at 1.6 s,
   as unhindered.
4. **With the secret, 16 holding for 6 s**: the job ended at 6.6 s, once they let
   go; meanwhile node 0 said "(16 callers part-way through the hello)" and node 1
   "no answer to the hello on a candidate port". The README's Trust paragraph says
   exactly this.
5. **Without a secret, a hello with the wrong counts**: node 0 ended at once
   naming both counts, as the design keeps the old order there ("Without the
   secret, any host that can reach the ports can join"); node 1 waited, noticing,
   until the 25 s limit.

The mesh accepts through the same function and proof (`wire.c:208`). The Trust
paragraph (README 1028-1037) is accurate in each case run. What it does not cover
is local: Q4.

## For the team work (kept apart)

Step E's small slot, the proposal's E2, is "rounded up to the cache line". These
lines live in the internal region, whose offsets every node computes alike, and
the region uses `OSOE_INTERNAL_LINE_BYTES` (128) rather than the compiler's cache
line, 128 on aarch64 and 64 on x86-64, "so that nodes of different architectures
in one job agree too" (`internal-region.h`). E's brief should say the internal
line, not `OSOE_CACHELINE`, or a mixed job - forbidden but unchecked - would put
into the wrong place. Step D's brief does not touch it.

## What is sound, and what convinced me

- **The library has no architecture in it** beyond a padding constant: no
  assembly, one conditional, fixed-width structures on the wire, the internal
  region by constants; the x86-64 and aarch64 builds export the same 3,595
  symbols; the x86-64 build passes every one-node test it can run, with no false
  pass among them.
- **The atomics hold on LL/SC**: the whole suite and three minutes of `mix`.
- **Kernel facilities are refused by name, early**, and the one-node path needs
  only futex2: measured for `ENOSYS` and `EPERM` io_uring, futex2, pidfd, memlock.
- **The trust fixes hold** as the record and the README state them, by five runs
  of my own rather than the suite's.
- **Slurm**: the 22 real multi-node captures parse right; the previous round's
  distribution and binding runs stand on an unchanged path.
- **Install**: relocatable, `lib64`-aware, the large-model statics included.

## Runs the lead could make elsewhere

1. **x86-64**, any host with Debian 13 (a cloud VM): `cmake --workflow --preset
   release`, then `ctest --preset release` twice. Confirms Q3 if everything passes
   but possibly `env.echo-uring`'s pins; a failure in `shmem.multinode.*`,
   `shmem.internode.*` or `tools.pacer.*` would be the first x86-only defect.
   With `sudo`, the `tier3` preset too.
2. **RHEL 10 or Rocky 10** (a VM): configure and build (expect success), `env.probe`
   (expect the io_uring checks to fail, naming creation and `EPERM`), the release
   suite (expect about 300 failures at `shmem_init`), then `sysctl
   kernel.io_uring_disabled=0` and the suite again (expect it whole). Confirms Q1.
3. **Huge pages**, in a VM rather than on the cluster since it changes a sysctl:
   with `vm.nr_hugepages=64`, an anonymous `MAP_HUGETLB` mapping now succeeds and
   a two-node job with `OSOE_AM_HUGEPAGES=1` still prints "(Invalid argument)".
   Confirms Q2's reading.
4. **Clang** in a Linux VM: `CC=clang cmake --workflow --preset release`; expect
   `shmem.strict-c11` to fail on `test-reply.c:278`. Confirms Q5.
5. **A Pi 4** (Cortex-A72, no LSE), if one is at hand: the release suite, for the
   LL/SC path on real cores.

## Sources

The tree at `08b3da0`: `README.md`, `docs/design/shmem-ethernet-design.md` §1,
§3.4, §6.2, `docs/design/team-resources-proposal.md` (E2),
`docs/prompts/phase1-step7-slice3-prompt.md`, `docs/decisions/2026-10-02.md`,
`docs/decisions/2026-10-02-release-fixes.md`, `docs/prompts/phase1-release-fixes-plan.md`,
`shmem/src/{listener,rendezvous,init,node,liveness,connection-area,ring,bootstrap,lock}.c`,
`shmem/src/internal-region.h`, `base/{ring-check,cacheline}.*`, `tools/stat/segment.c`,
`tools/wrapper/`, `cmake/CheckStrictC11.cmake`, `shmem/tests/CMakeLists.txt`,
`docs/design/evidence/pi-cluster/2026-09-28-review/slurm-env-*`. The previous
round's shim, `build/scratch/review-r3/x86/shim/`, and its clang method.
External (*documented*): Red Hat, RHEL 10.0 release notes, "Technology Preview
features" (io_uring); remirepo's liburing table (EL-10 2.12-1.el10); liburing's
`CHANGELOG` (2.5, 2.6); Linux v6.12 `mm/mmap.c`; `moby/profiles`
`seccomp/default.json`; moby PR 46762 and issue 47532.

Logs and scripts, outside the repository, in this session's scratchpad
(`.../scratchpad/q4/`): `x86-c1..4.log` and `x86-passed-v.log` (the x86 suite),
`classify.py`, `llsc-c1..2.log` and `llsc-mix.log`, `denied-c1..2.log` (Q1),
`clang-pass.py` with `cc-x86.json` and `cc-arm.json` (Q5), `pi-fixtures/` (the 22
replayed captures), `trust/` (`run.sh`, `hello.py`, `gc-race.sh`, the programs),
`probe/` (`hugetlb.c`, `split2.c`). The VM's copies (`/tmp/q4r`, 536 MB) and the
void earlier run (`/tmp/q4`) were removed when done; `/tmp/ll*` and `/tmp/sl*`,
from 2 October and not this review's, were left in place.
